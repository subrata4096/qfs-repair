
#include <iostream>
#include <fstream>
#include <sstream>
#include <cerrno>

extern "C" {
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
}

#include "libclient/KfsClient.h"
#include "libclient/KfsAttr.h"

using std::cout;
using std::cerr;
using std::endl;
using std::ifstream;
using std::string;
using std::vector;

KFS::KfsClient *gKfsClient;

int
main(int argc, char **argv)
{
    int chunkSizeInMB = 64;  //64MB chunkSize
    int numData = 6;
    int numParity = 3;
    int64_t duration1, duration2, duration3;

    bool help = false;
    char optchar;

    cout << "Usage: " << argv[0] << " -c <raw chunksize> -d <number of data chunks> -p <number of parity chunks> " << endl;
    while ((optchar = getopt(argc, argv, "hc:d:p:")) != -1) {
        switch (optchar) {
            case 'c':
                chunkSizeInMB = atoi(optarg);
                break;
            case 'd':
                numData = atoi(optarg);
                break;
            //case 'c':
            //    chunkSize = strtoll(optarg, NULL, 10);;
            //    break;
            case 'h':
                help = true;
                break;
            case 'p':
                numParity = atoi(optarg);
                break;
            default:
                cout << "Unrecognized flag " << optchar << endl;
                help = true;
                break;
        }
    }

    if (help) {
        cout << "Usage: " << argv[0] << " -c <raw chunksize> -d <number of data chunks> -p <number of parity chunks> "
             << endl;
        exit(0);
    }

    int chunkSize = chunkSizeInMB << 20;

    KFS::KfsClient::doDecodePerformanceTest(chunkSize, numData, numParity, duration1, duration2, duration3);
	
    std::cout <<  "For : " << numData << " + " << numParity << " decoding, serial : simulated-parallel : total-partial-serial decoding took = " << duration1  << " : " << duration2 << " : " << duration3 << endl;

    return 0;

}


