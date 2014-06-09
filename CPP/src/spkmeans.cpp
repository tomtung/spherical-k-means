/* File: spkmeans.cpp
 *
 * A parallel implementation of the Spherical K-Means algorithm using the
 * Galois library (http://iss.ices.utexas.edu/?p=projects/galois) and OpenMP.
 */

// PROGRAM VERSION
#define VERSION "0.1 (dev)"


#include <fstream>
#include <iostream>
#include <queue>
#include <string>
#include <vector>

#include "Galois/Galois.h"
#include "Galois/Timer.h"

#include "cluster_data.h"
#include "reader.h"
#include "vectors.h"


// return flags (for argument parsing)
#define RETURN_SUCCESS 0
#define RETURN_HELP 1
#define RETURN_VERSION 2
#define RETURN_ERROR -1

// default parameters
#define DEFAULT_K 2
#define DEFAULT_THREADS 2
#define Q_THRESHOLD 0.001
#define DEFAULT_DOC_FILE "docs"

// type of parallel implementations
#define RUN_NORMAL 0
#define RUN_GALOIS 1
#define RUN_OPENMP 2


using namespace std;



// Debug: prints the given vector (array) to std out.
void printVec(float *vec, int size)
{
    for(int i=0; i<size; i++)
        cout << vec[i] << " ";
    cout << endl;
}



// Prints a short message on how to use this program.
void printUsage()
{
    // $ ./spkmeans -d docfile -w wordfile -k 2 -t 2 --galois
    cout << "Usage: " << endl
         << " $ ./spkmeans [-d docfile] [-v vocabfile] [-k k] [-t numthreads] "
         << "[--galois OR --openmp]" << endl
         << "Other commands: " << endl
         << " $ ./spkmeans --help" << endl
         << " $ ./spkmeans --version" << endl;
}



// Applies the TXN scheme to each document vector of the given matrix.
// TXN effectively just normalizes each of the document vectors.
void txnScheme(float **doc_matrix, int dc, int wc)
{
    for(int i=0; i<dc; i++)
        vec_normalize(doc_matrix[i], wc);
}



// Returns the quality of the given partition by doing a dot product against
// its given concept vector.
float computeQuality(float **partition, int p_size, float *concept, int wc)
{
    float *sum_p = vec_sum(partition, wc, p_size);
    float quality = vec_dot(sum_p, concept, wc);
    delete sum_p;
    return quality;
}



// Returns the total quality of all partitions by summing the qualities of
// each individual partition.
float computeQuality(float ***partitions, int *p_sizes, float **concepts,
    int k, int wc)
{
    float quality = 0;
    for(int i=0; i<k; i++)
        quality += computeQuality(partitions[i], p_sizes[i], concepts[i], wc);
    return quality;
}



// Computes the cosine similarity value of the two given vectors (dv and cv).
float cosineSimilarity(float *dv, float *cv, int wc)
{
    return vec_dot(dv, cv, wc) / (vec_norm(dv, wc) * vec_norm(cv, wc));
}



// Computes the concept vector of the given partition. A partition is an array
// of document vectors, and the concept vector will be allocated and populated.
float* computeConcept(float **partition, int p_size, int wc)
{
    float *cv = vec_sum(partition, wc, p_size);
    vec_multiply(cv, wc, (1.0 / wc));
    vec_divide(cv, wc, vec_norm(cv, wc));
    return cv;
}



// Runs the spherical k-means algorithm on the given sparse matrix D and
// clusters the data into k partitions.
ClusterData runSPKMeans(float **doc_matrix, unsigned int k, int dc, int wc)
{
    // keep track of the run time for this algorithm
    Galois::Timer timer;
    timer.start();


    // apply the TXN scheme on the document vectors (normalize them)
    txnScheme(doc_matrix, dc, wc);


    // initialize the data arrays; keep track of the arrays locally
    ClusterData data(k, dc, wc);
    float ***partitions = data.partitions;
    int *p_sizes = data.p_sizes;
    float **concepts = data.concepts;


    // create the first arbitrary partitioning
    int split = floor(dc / k);
    cout << "Split = " << split << endl;
    int base = 1;
    for(int i=0; i<k; i++) {
        int top = base + split - 1;
        if(i == k-1)
            top = dc;

        int p_size = top - base + 1;
        p_sizes[i] = p_size;
        cout << "Created new partition of size " << p_size << endl;

        partitions[i] = new float*[p_size];
        for(int j=0; j<p_size; j++)
            partitions[i][j] = doc_matrix[base + j - 1];

        base = base + split;
    }


    // compute concept vectors
    for(int i=0; i<k; i++)
        concepts[i] = computeConcept(partitions[i], p_sizes[i], wc);


    // compute initial quality of the partitions
    float quality = computeQuality(partitions, p_sizes, concepts, k, wc);
    cout << "Initial quality: " << quality << endl;


    // keep track of all individual component times for analysis
    Galois::Timer ptimer;
    Galois::Timer ctimer;
    Galois::Timer qtimer;
    float p_time = 0;
    float c_time = 0;
    float q_time = 0;

    // do spherical k-means loop
    float dQ = Q_THRESHOLD * 10;
    int iterations = 0;
    while(dQ > Q_THRESHOLD) {
        iterations++;

        // compute new partitions based on old concept vectors
        ptimer.start();
        vector<float*> *new_partitions = new vector<float*>[k];
        for(int i=0; i<k; i++)
            new_partitions[i] = vector<float*>();
        for(int i=0; i<dc; i++) {
            int cIndx = 0;
            float cVal = cosineSimilarity(doc_matrix[i], concepts[0], wc);
            for(int j=1; j<k; j++) {
                float new_cVal = cosineSimilarity(doc_matrix[i], concepts[j], wc);
                if(new_cVal > cVal) {
                    cVal = new_cVal;
                    cIndx = j;
                }
            }
            new_partitions[cIndx].push_back(doc_matrix[i]);
        }
        ptimer.stop();
        p_time += ptimer.get();

        // transfer the new partitions to the partitions array
        data.clearPartitions();
        for(int i=0; i<k; i++) {
            partitions[i] = new_partitions[i].data();
            p_sizes[i] = new_partitions[i].size();
        }

        // compute new concept vectors
        ctimer.start();
        data.clearConcepts();
        for(int i=0; i<k; i++)
            concepts[i] = computeConcept(partitions[i], p_sizes[i], wc);
        ctimer.stop();
        c_time += ctimer.get();

        // compute quality of new partitioning
        qtimer.start();
        float n_quality = computeQuality(partitions, p_sizes, concepts, k, wc);
        dQ = n_quality - quality;
        quality = n_quality;
        qtimer.stop();
        q_time += qtimer.get();

        cout << "Quality: " << quality << " (+" << dQ << ")" << endl;
    }


    // report runtime statistics
    timer.stop();
    float time_in_ms = timer.get();
    cout << "Done in " << time_in_ms / 1000
         << " seconds after " << iterations << " iterations." << endl;
    float total = p_time + c_time + q_time;
    if(total == 0)
        cout << "No time stats available: program finished too fast." << endl;
    else {
        cout << "Timers (ms): " << endl
             << "   partition [" << p_time << "] ("
                << (p_time/total)*100 << "%)" << endl
             << "   concepts [" << c_time << "] ("
                << (c_time/total)*100 << "%)" << endl
             << "   quality [" << q_time << "] ("
                << (q_time/total)*100 << "%)" << endl;
    }


    // return the resulting partitions and concepts in the ClusterData struct
    return data;
}



// Displays the results of each partition. If a words list is provided,
// the top num_to_show words will be displayed for each partition.
// Otherwise, if words is a null pointer, only the indices will be shown.
void displayResults(ClusterData *data, char **words, int num_to_show = 10)
{
    // make sure num_to_show doesn't exceed the actual word count
    if(num_to_show > data->wc)
        num_to_show = data->wc;

    // for each partition, sum the weights of each word, and show the top
    //  words that occur in the partition:
    for(int i=0; i<(data->k); i++) {
        cout << "Partition #" << (i+1) << ":" << endl;
        // sum the weights
        float *sum = vec_sum(data->partitions[i], data->wc, data->p_sizes[i]);

        // sort this sum using C++ priority queue (keeping track of indices)
        vector<float> values(sum, sum + data->wc);
        priority_queue<pair<float, int>> q;
        for(int i=0; i<values.size(); i++)
            q.push(pair<float, int>(values[i], i));

        // show top num_to_show words
        for(int i=0; i<num_to_show; i++) {
            int index = q.top().second;
            if(words != 0)
                cout << "   " << words[index] << endl;
            else
                cout << "   " << index << endl;
            q.pop();
        }
    }
}



// Takes argc and argv from program input and parses the parameters to set
// values for k (number of clusters) and num_threads (the maximum number
// of threads for Galois to use).
// Returns -1 on fail (provided file doesn't exist), else 0 on success.
int processArgs(int argc, char **argv,
    string *doc_fname, string *vocab_fname,
    unsigned int *k, unsigned int *num_threads, unsigned int *run_type)
{
    // set defaults before proceeding to check arguments
    *doc_fname = DEFAULT_DOC_FILE;
    *vocab_fname = "";
    *k = DEFAULT_K;
    *num_threads = DEFAULT_THREADS;
    *run_type = RUN_NORMAL;

    // check arguments: expected command as follows:
    // $ ./spkmeans -d docfile -w wordfile -k 2 -t 2 --galois
    for(int i=1; i<argc; i++) {
        string arg(argv[i]);

        // if flag is --help, return 1 to print usage instructions
        if(arg == "--help" || arg == "-h")
            return RETURN_HELP;
        // if flag is --version, return 2 to print version number
        else if(arg == "--version" || arg == "-V")
            return RETURN_VERSION;

        // if the flag was to run as galois or openmp, set the run type
        else if(arg == "--galois")
            *run_type = RUN_GALOIS;
        else if(arg == "--openmp")
            *run_type = RUN_OPENMP;

        // otherwise, check the given flag value
        else {
            i++;
            if(arg == "-d") // document file
                *doc_fname = string(argv[i]);
            else if(arg == "-w" || arg == "-v") // words file
                *vocab_fname = string(argv[i]);
            else if(arg == "-k") // size of k
                *k = atoi(argv[i]);
            else if(arg == "-t") // number of threads
                *num_threads = atoi(argv[i]);
        }
    }

    // check that the document file exists - if not, return error
    ifstream test(doc_fname->c_str());
    if(!test.good()) {
        cout << "Error: file \"" << *doc_fname << "\" does not exist." << endl;
        test.close();
        return RETURN_ERROR;
    }
    test.close();

    return RETURN_SUCCESS;
}



// main: set up Galois and start the clustering process.
int main(int argc, char **argv)
{
    // get file names, and set up k and number of threads
    string doc_fname, vocab_fname;
    unsigned int k, num_threads, run_type;
    int retval = processArgs(
        argc, argv,
        &doc_fname, &vocab_fname, &k, &num_threads, &run_type);
    if(retval == RETURN_ERROR) {
        printUsage();
        return -1;
    }
    else if(retval == RETURN_HELP) {
        printUsage();
        return 0;
    }
    else if(retval == RETURN_VERSION) {
        cout << "Version: " << VERSION << endl;
        return 0;
    }

    // tell Galois the max thread count
    Galois::setActiveThreads(num_threads);
    num_threads = Galois::getActiveThreads();
    cout << "Running SPK Means on \"" << doc_fname << "\" with k=" << k
         << " (" << num_threads << " threads)." << endl;

    // set up the sparse matrix
    int dc, wc;
    float **D = readDocFile(doc_fname.c_str(), dc, wc);
    cout << dc << " documents, " << wc << " words." << endl;

    // run spherical k-means on the given sparse matrix D
    ClusterData data = runSPKMeans(D, k, dc, wc);

    char **words = readWordsFile(vocab_fname.c_str(), wc);
    displayResults(&data, words, 10);

    return 0;
}
