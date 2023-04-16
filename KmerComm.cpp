#include "KmerComm.h"
#include <numeric>
#include <algorithm>
#include <cmath>

KmerCountMap GetKmerCountMapKeys(const Vector <String>& myreads, SharedPtr<CommGrid> commgrid)
{
    /*
     * This function initializes an associative container of k-mers on each processor,
     * whose keys correspond to the reliable k-mers that have been assigned to that processor.
     */

    KmerCountMap kmermap;

    int myrank = commgrid->GetRank();
    int nprocs = commgrid->GetSize();
    size_t numreads = myreads.size();

    /*
     * When we use the word "k-mer" there are often actually two different things
     * we are referring to, which can lead to confusion. One thing we're interested
     * in is simply that a k-mer exists in a certain read at a certain position.
     * Let's call them "seed k-mers". The other kind of k-mer is simply a specific
     * sequence of k nucleotides. This latter kind of k-mer can appear multiple times
     * in different reads and even in the same read. We still refer to it as one k-mer,
     * because in this case we only care about its particular sequence. We will
     * call these just "k-mers". Hence, multiple distinct "seed k-mers" can in actual
     * fact be the same "k-mer".
     */

    /*
     * The function ForeachKmer is a template function. The underlying function
     * scans through each "seed k-mer" in every sequence passed to it (i.e. myreads)
     * and calls the handler (a function object whose operator() operates on a TKmer object)
     * on every "seed k-mer". We use use this because there are multiple different
     * circumstances where we want to enumerate all seed k-mers and do something to them,
     * but we don't want to have to rewrite the loops everytime we want to do
     * something different with the seed k-mers.
     *
     * We first want to estimate the total number of unique "k-mers" in
     * our dataset without having to store all the k-mers in memory (e.g. in a Set object),
     * which we can do using the HyperLogLog count-distinct data structure. We therefore
     * initialize a HyperLogLog object, and attach it to a function object called KmerEstimateHandler,
     * which defines the primitive operation we are interested in, namely: adding a k-mer
     * to the HyperLogLog counter.
     */
    HyperLogLog hll(12);
    KmerEstimateHandler estimator(hll);
    ForeachKmer(myreads, estimator); /* Adds each representative seed k-mer to HyperLogLog */

    /*
     * The KmerEstimateHandler object took the HyperLogLog object by reference, so
     * we can immediately access its results in the current scope.
     *
     * We first want to merge all the individual results across all the processors
     * in the grid communicator, and then we can get an estimate for the total
     * number of unique k-mers in the dataset.
     */
    hll.ParallelMerge(commgrid->GetWorld());
    size_t cardinality_estimate = static_cast<size_t>(std::ceil(hll.Estimate()));

    if (!myrank) std::cout << "Estimate a total of " << cardinality_estimate << " k-mers" << std::endl;

    /*
     * Remember what the final goal is: we want to find all the "seed k-mers" whose corresponding
     * "k-mer" appears in the sequencing reads between LOWER_KMER_FREQ and UPPER_KMER_FREQ different
     * times (inclusive). We call these "k-mers" "reliable k-mers". Eventually, we also want to
     * record all the read ids and read positions for each "reliable k-mer". We start by figuring
     * out which "k-mers" fit the above criteria, so that we can pass through all the reads again
     * and just collect the read ids and positions when a "reliable k-mer" is found.
     *
     * This requires partitioning the k-mers so each processor is responsible for a distinct
     * "k-mer". That way, when we are counting the number of times a "k-mer" appears in order
     * to determine whether it is reliable or not, there is a single processor responsible
     * for counting that "k-mer".
     *
     * We do this using the KmerPartitionHandler function object, which assigns k-mers to processor
     * ranks using an injective function based on the hash of the k-mer. The object takes
     * a vector of "k-mer buckets", one for each processor destination, so that we can put each
     * "seed k-mer" found in the local read set into its correct outgoing bucket before
     * peforming an Alltoall communication. The result of the Altoall will be that each processor
     * receives a list of "seed k-mers" assigned to it by the partitioner, such that each "k-mer"
     * has a unique processor destination id.
     *
     */

    Vector<Vector<TKmer>> kmerbuckets(nprocs); /* outgoing k-mer buckets */
    KmerPartitionHandler partitioner(kmerbuckets);
    ForeachKmer(myreads, partitioner); /* adds each representative seed k-mer to its proper outgoing bucket */

    /*
     * Now that we know where all the k-mers need to be sent to, we just need pack
     * the data into a contiguous buffer and compute the corresponding Alltoallv
     * communication parameters so that we can send the k-mers around.
     */

    Vector<MPI_Count_type> sendcnt(nprocs); /* sendcnt[i] is number of bytes of k-mers this process sends to process i */
    Vector<MPI_Count_type> recvcnt(nprocs); /* recvnct[i] is number of bytes of k-mers this process receives from process i */
    Vector<MPI_Displ_type> sdispls(nprocs); /* sdispls[i] = sdispls[i-1] + sendcnt[i] */
    Vector<MPI_Displ_type> rdispls(nprocs); /* rdispls[i] = rdispls[i-1] + recvcnt[i] */

    /*
     * Initialize sendcnt parameter for local process.
     */
    for (int i = 0; i < nprocs; ++i)
    {
        /*
         * Each k-mer is a fixed number of bytes (TKmer::N_BYTES),
         * usually 16 for 32 < k < 64.
         */
        sendcnt[i] = kmerbuckets[i].size() * TKmer::N_BYTES;
    }

    /*
     * Let every process know how many k-mers it will be receiving
     * from each other process.
     */
    MPI_ALLTOALL(sendcnt.data(), 1, MPI_COUNT_TYPE, recvcnt.data(), 1, MPI_COUNT_TYPE, commgrid->GetWorld());

    /*
     * Initialize displacement parameters now that we know both send and receive counts.
     */
    sdispls.front() = rdispls.front() = 0;

    std::partial_sum(sendcnt.begin(), sendcnt.end()-1, sdispls.begin()+1);
    std::partial_sum(recvcnt.begin(), recvcnt.end()-1, rdispls.begin()+1);

    /*
     * Need total send and receive buffer sizes in order to allocate
     * the memory.
     */
    int64_t totsend = std::accumulate(sendcnt.begin(), sendcnt.end(), 0);
    int64_t totrecv = std::accumulate(recvcnt.begin(), recvcnt.end(), 0);

    /*
     * Pack send buffer.
     */
    Vector<uint8_t> sendbuf(totsend, 0);

    for (int i = 0; i < nprocs; ++i)
    {
        assert(kmerbuckets[i].size() == (sendcnt[i] / TKmer::N_BYTES));

        /*
         * Get starting adddress of buffer space for kmerbuckets[i].
         */
        uint8_t *addrs2fill = sendbuf.data() + sdispls[i];

        for (MPI_Count_type j = 0; j < kmerbuckets[i].size(); ++j)
        {
            /*
             * Copy jth k-mer to be sent to ith processor into the
             * next available spot in the buffer.
             */
            (kmerbuckets[i][j]).CopyDataInto(addrs2fill);
            addrs2fill += TKmer::N_BYTES;
        }

        /*
         * Now that the k-mers to be sent to the ith processor
         * have been fully packed into the buffer, we don't
         * need them anymore.
         */
        kmerbuckets[i].clear();
    }

    /*
     * Allocate receive buffer.
     */
    Vector<uint8_t> recvbuf(totrecv, 0);

    /*
     * Send all the k-mers around.
     */
    MPI_ALLTOALLV(sendbuf.data(), sendcnt.data(), sdispls.data(), MPI_BYTE, recvbuf.data(), recvcnt.data(), rdispls.data(), MPI_BYTE, commgrid->GetWorld());

    /*
     * Get actual number of k-mer seeds received.
     */
    uint64_t numkmerseeds = totrecv / TKmer::N_BYTES;

    uint8_t *addrs2read = recvbuf.data();

    for (uint64_t i = 0; i < numkmerseeds; ++i)
    {
        TKmer mer(addrs2read);
        addrs2read += TKmer::N_BYTES;

        if (kmermap.find(mer) == kmermap.end())
        {
            kmermap.insert({mer, KmerCountEntry()});
        }
    }

    return kmermap;
}

void GetKmerCountMapValues(const Vector<String>& myreads, KmerCountMap& kmermap, SharedPtr<CommGrid> commgrid)
{
    return;
}


