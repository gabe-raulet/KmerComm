#include "FastaIndex.h"
#include "Logger.h"
#include <cstring>
#include <iterator>
#include <algorithm>
#include <numeric>
#include <fstream>
#include <sstream>
#include <iostream>

typedef typename FastaIndex::faidx_record_t faidx_record_t;

faidx_record_t GetFaidxRecord(const String& line, Vector<String>& names)
{
    String name;
    faidx_record_t record;
    std::istringstream(line) >> name >> record.len >> record.pos >> record.bases;
    names.push_back(name);
    return record;
}

FastaIndex::FastaIndex(const String& fasta_fname, SharedPtr<CommGrid> commgrid) : commgrid(commgrid), fasta_fname(fasta_fname)
{
    int nprocs = commgrid->GetSize();
    int myrank = commgrid->GetRank();

    Vector<MPI_Count_type> sendcounts; /* MPI_Scatterv sendcounts for faidx_record_t records (root only) */
    Vector<MPI_Displ_type> displs;     /* MPI_Scatterv displs for faidx_record_t records (root only)     */
    MPI_Count_type recvcount;          /* MPI_Scatterv recvcount for faidx_record_t records              */

    Vector<String> root_names;

    if (myrank == 0)
    {
        String line;
        std::ifstream filestream(GetFaidxFilename());

        while (std::getline(filestream, line))
        {
            allrecords.push_back(GetFaidxRecord(line, root_names));
        }

        filestream.close();

        MPI_Count_type num_records = allrecords.size();

        sendcounts.resize(nprocs);
        displs.resize(nprocs);

        displs.front() = 0;

        MPI_Count_type records_per_proc = num_records / nprocs;

        std::fill_n(sendcounts.begin(), nprocs-1, records_per_proc);

        sendcounts.back() = num_records - (nprocs-1) * records_per_proc;

        std::partial_sum(sendcounts.begin(), sendcounts.end()-1, displs.begin()+1);
    }

    /*
     * Root process tells each process how many faidx_record_t records it will be sent.
     */
    MPI_SCATTER(sendcounts.data(), 1, MPI_COUNT_TYPE, &recvcount, 1, MPI_COUNT_TYPE, 0, commgrid->GetWorld());

    myrecords.resize(recvcount);

    MPI_Datatype faidx_dtype_t;
    MPI_Type_contiguous(3, MPI_SIZE_T, &faidx_dtype_t);
    MPI_Type_commit(&faidx_dtype_t);
    MPI_SCATTERV(allrecords.data(), sendcounts.data(), displs.data(), faidx_dtype_t, myrecords.data(), recvcount, faidx_dtype_t, 0, commgrid->GetWorld());
    MPI_Type_free(&faidx_dtype_t);
}

Vector<String> FastaIndex::GetReadsFromRecords(const Vector<faidx_record_t>& records)
{
    Vector<String> reads;

    size_t num_records = records.size();

    reads.reserve(num_records);

    const faidx_record_t& first_record = records.front();
    const faidx_record_t& last_record = records.back();

    MPI_Offset startpos = first_record.pos;
    MPI_Offset endpos = last_record.pos + last_record.len + (last_record.len / last_record.bases);

    MPI_File fh;
    MPI_File_open(commgrid->GetWorld(), fasta_fname.c_str(), MPI_MODE_RDONLY, MPI_INFO_NULL, &fh);

    MPI_Offset filesize;
    MPI_File_get_size(fh, &filesize);

    endpos = std::min(endpos, filesize);
    // endpos = std::max(endpos, filesize);

    MPI_Count_type mychunksize = endpos - startpos;

    Vector<char> mychunk(mychunksize);

    MPI_FILE_READ_AT_ALL(fh, startpos, mychunk.data(), mychunksize, MPI_CHAR, MPI_STATUS_IGNORE);
    MPI_File_close(&fh);

    size_t maxlen, totbases, offset = 0;

    MPI_Exscan(&num_records, &offset, 1, MPI_SIZE_T, MPI_SUM, commgrid->GetWorld());

    maxlen   = std::accumulate(records.begin(), records.end(), static_cast<size_t>(0), [](size_t l, const faidx_record_t& rec) { return std::max(l, rec.len); });
    totbases = std::accumulate(records.begin(), records.end(), static_cast<size_t>(0), [](size_t l, const faidx_record_t& rec) { return l + rec.len; });

    char *seqbuf = new char[maxlen];

    double t0, t1;
    t0 = MPI_Wtime();
    for (auto itr = records.cbegin(); itr != records.cend(); ++itr)
    {
        size_t locpos = 0;
        ptrdiff_t chunkpos = itr->pos - startpos;
        ptrdiff_t remain = itr->len;
        char *bufptr = seqbuf;

        while (remain > 0)
        {
            size_t cnt = std::min(itr->bases, static_cast<size_t>(remain));
            std::memcpy(bufptr, &mychunk.data()[chunkpos + locpos], cnt);
            bufptr += cnt;
            remain -= cnt;
            locpos += (cnt+1);
        }

        reads.emplace_back(seqbuf, itr->len);
    }
    t1 = MPI_Wtime();

    double mbspersecond = (totbases / 1048576.0) / (t1-t0);
    Logger logger(commgrid);
    logger() << std::fixed << std::setprecision(2) << mbspersecond << " megabytes parsed per second";
    logger.Flush("FASTA parsing rates:");

    delete[] seqbuf;
    return reads;
}

Vector<String> FastaIndex::GetMyReads()
{
    Vector<String> reads = GetReadsFromRecords(myrecords);

    size_t mynumreads = reads.size();
    size_t mytotbases = std::accumulate(myrecords.begin(), myrecords.end(), static_cast<size_t>(0), [](size_t cur, const faidx_record_t& rec) { return cur + rec.len; });
    double myavglen = static_cast<double>(mytotbases) / static_cast<double>(mynumreads);

    size_t myreadoffset;
    MPI_Exscan(&mynumreads, &myreadoffset, 1, MPI_SIZE_T, MPI_SUM, commgrid->GetWorld());
    if (commgrid->GetRank() == 0) myreadoffset = 0;

    Logger logger(commgrid);
    logger() << std::fixed << std::setprecision(2) << " sequence range [" << myreadoffset << ".." << myreadoffset+mynumreads << "). ~" << myavglen << " nucleotides per read. (" << static_cast<double>(mytotbases) / (1024.0 * 1024.0) << " megabytes)";
    logger.Flush("FASTA distributed among process ranks:");

    return reads;
}
