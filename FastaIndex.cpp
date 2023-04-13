#include "FastaIndex.h"
#include <cstring>
#include <iterator>
#include <algorithm>
#include <numeric>
#include <fstream>
#include <sstream>
#include <iostream>

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

    Vector<MPI_Count> sendcounts; /* MPI_Scatterv sendcounts for faidx_record_t records (root only) */
    Vector<MPI_Aint> displs;      /* MPI_Scatterv displs for faidx_record_t records (root only)     */
    MPI_Count recvcount;          /* MPI_Scatterv recvcount for faidx_record_t records              */

    Vector<faidx_record_t> root_records;
    Vector<String> root_names;

    if (myrank == 0)
    {
        String line;
        std::ifstream filestream(GetFaidxFilename());

        while (std::getline(filestream, line))
        {
            root_records.push_back(GetFaidxRecord(line, root_names));
        }

        filestream.close();

        MPI_Count num_records = root_records.size();

        sendcounts.resize(nprocs);
        displs.resize(nprocs);

        displs.front() = 0;

        MPI_Count records_per_proc = num_records / nprocs;

        std::fill_n(sendcounts.begin(), nprocs-1, records_per_proc);

        sendcounts.back() = num_records - (nprocs-1) * records_per_proc;

        std::partial_sum(sendcounts.begin(), sendcounts.end()-1, displs.begin()+1);
    }

    /*
     * Root process tells each process how many faidx_record_t records it will be sent.
     */
    MPI_Scatter_c(sendcounts.data(), 1, MPI_COUNT, &recvcount, 1, MPI_COUNT, 0, commgrid->GetWorld());

    records.resize(recvcount);

    MPI_Datatype faidx_dtype_t;
    MPI_Type_contiguous(3, MPI_UNSIGNED_LONG, &faidx_dtype_t);
    MPI_Type_commit(&faidx_dtype_t);
    MPI_Scatterv_c(root_records.data(), sendcounts.data(), displs.data(), faidx_dtype_t, records.data(), recvcount, faidx_dtype_t, 0, commgrid->GetWorld());
    MPI_Type_free(&faidx_dtype_t);
}

Vector<String> FastaIndex::GetMyReads(const FastaIndex& index)
{
    Vector<String> reads;

    const Vector<faidx_record_t>& records = index.getrecords();
    unsigned long num_records = records.size();

    reads.reserve(num_records);

    const faidx_record_t& first_record = records.front();
    const faidx_record_t& last_record = records.back();

    MPI_Offset startpos = first_record.pos;
    MPI_Offset endpos = last_record.pos + last_record.len + (last_record.len / last_record.bases);

    MPI_File fh;
    MPI_File_open(index.commgrid->GetWorld(), index.fasta_fname.c_str(), MPI_MODE_RDONLY, MPI_INFO_NULL, &fh);

    MPI_Offset filesize;
    MPI_File_get_size(fh, &filesize);

    endpos = std::max(endpos, filesize);

    MPI_Count mychunksize = endpos - startpos;

    Vector<char> mychunk(mychunksize);

    MPI_File_read_at_all_c(fh, startpos, mychunk.data(), mychunksize, MPI_CHAR, MPI_STATUS_IGNORE);
    MPI_File_close(&fh);

    unsigned long maxlen, offset = 0;

    MPI_Exscan(&num_records, &offset, 1, MPI_UNSIGNED_LONG, MPI_SUM, index.commgrid->GetWorld());

    maxlen = std::accumulate(records.begin(), records.end(), 0, [](unsigned long l, const faidx_record_t& rec) { return l > rec.len? l : rec.len; });

    char *seqbuf = new char[maxlen];

    for (auto itr = records.cbegin(); itr != records.cend(); ++itr)
    {
        unsigned long locpos = 0;
        signed long chunkpos = itr->pos - startpos;
        signed long remain = itr->len;
        char *bufptr = seqbuf;

        while (remain > 0)
        {
            unsigned long cnt = std::min(itr->bases, static_cast<unsigned long>(remain));
            std::memcpy(bufptr, &mychunk.data()[chunkpos + locpos], cnt);
            bufptr += cnt;
            remain -= cnt;
            locpos += (cnt+1);
        }

        reads.emplace_back(seqbuf, itr->len);
    }

    delete[] seqbuf;

    return reads;
}

void FastaIndex::PrintInfo() const
{
    unsigned long mynumreads = records.size();
    unsigned long mytotbases = std::accumulate(records.begin(), records.end(), 0, [](unsigned long cur, const faidx_record_t& rec) { return cur + rec.len; });
    double myavglen = static_cast<double>(mytotbases) / static_cast<double>(mynumreads);

    std::ostringstream ss;

    ss << "P(" << commgrid->GetRankInProcCol() << ", " << commgrid->GetRankInProcRow() << ") will store " << mynumreads << " reads with an average length of " << myavglen << " nucleotides each";

    String myline = ss.str();
    char const *sendbuf = myline.c_str();
    MPI_Count sendcnt = myline.size();

    int myrank = commgrid->GetRank();
    int nprocs = commgrid->GetSize();

    Vector<MPI_Count> recvcnts;
    Vector<MPI_Aint> displs;

    if (myrank == 0) { recvcnts.resize(nprocs); displs.resize(nprocs); }

    MPI_Gather_c(&sendcnt, 1, MPI_COUNT, recvcnts.data(), 1, MPI_COUNT, 0, commgrid->GetWorld());

    char *recvbuf = NULL;

    if (myrank == 0)
    {
        displs.front() = 0;
        std::partial_sum(recvcnts.begin(), recvcnts.end()-1, displs.begin()+1);
        recvbuf = new char[displs.back() + recvcnts.back()];
    }

    MPI_Gatherv_c(sendbuf, sendcnt, MPI_CHAR, recvbuf, recvcnts.data(), displs.data(), MPI_CHAR, 0, commgrid->GetWorld());

    if (myrank == 0)
    {
        for (int i = 0; i < nprocs; ++i)
        {
            String line = String(recvbuf + displs[i], recvcnts[i]);
            std::cerr << line << std::endl;
        }

        delete[] recvbuf;
    }
}
