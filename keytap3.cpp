/*! \file keytap3.cpp
 *  \brief Fully automated acoustic keyboard eavesdropping
 *  \author Georgi Gerganov
 */

#include "common.h"
#include "constants.h"
#include "subbreak3.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>

#define MY_DEBUG

using TSampleInput          = TSampleF;
using TSample               = TSampleI16;
using TWaveform             = TWaveformI16;
using TWaveformView         = TWaveformViewI16;
using TKeyPressData         = TKeyPressDataI16;
using TKeyPressCollection   = TKeyPressCollectionI16;

int main(int argc, char ** argv) {
    printf("Usage: %s record.kbd n-gram-dir [-FN] [-fN]\n", argv[0]);
    printf("    -FN - select filter type, (0 - none, 1 - first order high-pass, 2 - second order high-pass)\n");
    printf("    -fN - cutoff frequency in Hz\n");
    if (argc < 3) {
        return -1;
    }

    const int64_t sampleRate = kSampleRate;

    const auto argm = parseCmdArguments(argc, argv);
    const int filterId      = argm.count("F") == 0 ? EAudioFilter::FirstOrderHighPass : std::stoi(argm.at("F"));
    const int freqCutoff_Hz = argm.count("f") == 0 ? kFreqCutoff_Hz : std::stoi(argm.at("f"));

    TWaveform waveformInput;
    {
        TWaveformF waveformInputF;
        printf("[+] Loading recording from '%s'\n", argv[1]);
        if (readFromFile<TSampleF>(argv[1], waveformInputF) == false) {
            printf("Specified file '%s' does not exist\n", argv[1]);
            return -1;
        } else {
            printf("[+] Filtering waveform with filter type = %d and cutoff frequency = %d Hz\n", filterId, freqCutoff_Hz);
            ::filter(waveformInputF, (EAudioFilter) filterId, freqCutoff_Hz, kSampleRate);

            printf("[+] Converting waveform to i16 format ...\n");
            if (convert(waveformInputF, waveformInput) == false) {
                printf("Conversion failed\n");
                return -4;
            }
        }
    }

    printf("[+] Loaded recording: of %d samples (sample size = %d bytes)\n", (int) waveformInput.size(), (int) sizeof(TSample));
    printf("    Size in memory:          %g MB\n", (float)(sizeof(TSample)*waveformInput.size())/1024/1024);
    printf("    Sample size:             %d\n", (int) sizeof(TSample));
    printf("    Total number of samples: %d\n", (int) waveformInput.size());
    printf("    Recording length:        %g seconds\n", (float)(waveformInput.size())/sampleRate);

    TKeyPressCollection keyPresses;
    {
        const auto tStart = std::chrono::high_resolution_clock::now();

        printf("[+] Searching for key presses\n");

        TWaveform waveformMax;
        TWaveform waveformThreshold;
        if (findKeyPresses(getView(waveformInput, 0), keyPresses, waveformThreshold, waveformMax, 8.0, 512, 2*1024, true) == false) {
            printf("Failed to detect keypresses\n");
            return -2;
        }

        const auto tEnd = std::chrono::high_resolution_clock::now();

        printf("[+] Detected a total of %d potential key presses\n", (int) keyPresses.size());
        printf("[+] Search took %4.3f seconds\n", toSeconds(tStart, tEnd));
    }

    const int n = keyPresses.size();

    TSimilarityMap similarityMap;
    {
        const auto tStart = std::chrono::high_resolution_clock::now();

        printf("[+] Calculating CC similarity map\n");

        if (calculateSimilartyMap(2*256, 3*32, 2*256 - 128, keyPresses, similarityMap) == false) {
            printf("Failed to calculate similariy map\n");
            return -3;
        }

        const auto tEnd = std::chrono::high_resolution_clock::now();

        printf("[+] Calculation took %4.3f seconds\n", toSeconds(tStart, tEnd));

        const int ncc = std::min(32, n);
        for (int j = 0; j < ncc; ++j) {
            printf("%2d: ", j);
            for (int i = 0; i < ncc; ++i) {
                printf("%6.3f ", similarityMap[j][i].cc);
            }
            printf("\n");
        }
        printf("\n");

        auto minCC = similarityMap[0][1].cc;
        auto maxCC = similarityMap[0][1].cc;
        for (int j = 0; j < n - 1; ++j) {
            for (int i = j + 1; i < n; ++i) {
                minCC = std::min(minCC, similarityMap[j][i].cc);
                maxCC = std::max(maxCC, similarityMap[j][i].cc);
            }
        }

        printf("[+] Similarity map: min = %g, max = %g\n", minCC, maxCC);
    }

    Cipher::TFreqMap freqMap6;
    {
        const auto tStart = std::chrono::high_resolution_clock::now();

        printf("[+] Loading n-grams from '%s'\n", argv[2]);

        if (Cipher::loadFreqMapBinary((std::string(argv[2]) + "/ggwords-6-gram.dat.binary").c_str(), freqMap6) == false) {
            return -5;
        }

        const auto tEnd = std::chrono::high_resolution_clock::now();

        printf("[+] Loading took %4.3f seconds\n", toSeconds(tStart, tEnd));
    }

    {
        Cipher::Processor processor;

        Cipher::TParameters params;
        params.maxClusters = 29;
        params.wEnglishFreq = 20.0;
        params.nHypothesesToKeep = std::max(100, 2100 - 10*std::min(200, std::max(0, ((int) keyPresses.size() - 100))));
        processor.init(params, freqMap6, similarityMap);

        printf("[+] Attempting to recover the text from the recording ...\n");

        std::vector<Cipher::TResult> clusterings;

        // clustering
        {
            const auto tStart = std::chrono::high_resolution_clock::now();

            for (int nIter = 0; nIter < 16; ++nIter) {
                auto clusteringsCur = processor.getClusterings(params, 32);

                for (int i = 0; i < (int) clusteringsCur.size(); ++i) {
                    printf("[+] Clustering %d: pClusters = %g\n", i, clusteringsCur[i].pClusters);
                    clusterings.push_back(std::move(clusteringsCur[i]));
                }

                params.maxClusters = 29 + 4*(nIter + 1);
                processor.init(params, freqMap6, similarityMap);
            }

            const auto tEnd = std::chrono::high_resolution_clock::now();
            printf("[+] Clustering took %4.3f seconds\n", toSeconds(tStart, tEnd));
        }

        params.hint.clear();
        params.hint.resize(n, -1);

        [[maybe_unused]] int nConverged = 0;
        while (true) {
            // beam search
            {
                for (int i = 0; i < (int) clusterings.size(); ++i) {
                    Cipher::beamSearch(params, freqMap6, clusterings[i]);
                    printf("%8.3f %8.3f ", clusterings[i].p, clusterings[i].pClusters);
                    Cipher::printDecoded(clusterings[i].clusters, clusterings[i].clMap, params.hint);
                    Cipher::refineNearby(params, freqMap6, clusterings[i]);
                }
            }
            break;

            // hint refinement
            //{
            //    const auto tStart = std::chrono::high_resolution_clock::now();

            //    std::vector<std::map<int, int>> nOccurances(n);
            //    for (int i = 0; i < (int) clusterings.size(); ++i) {
            //        for (int j = 0; j < n; ++j) {
            //            nOccurances[j][Cipher::decode(clusterings[i].clusters, j, clusterings[i].clMap, params.hint)]++;
            //        }
            //    }

            //    // for each position j determine the most frequent cluster
            //    struct Entry {
            //        int pos;
            //        TLetter letter;
            //        int nOccurances;
            //    };

            //    std::vector<Entry> mostFrequentClusters(n);
            //    for (int j = 0; j < n; ++j) {
            //        int maxOccurances = 0;
            //        for (const auto& p : nOccurances[j]) {
            //            if (p.second > maxOccurances) {
            //                maxOccurances = p.second;
            //                mostFrequentClusters[j].pos = j;
            //                mostFrequentClusters[j].letter = p.first;
            //                mostFrequentClusters[j].nOccurances = p.second;
            //            }
            //        }
            //    }

            //    std::sort(mostFrequentClusters.begin(), mostFrequentClusters.end(), [](const Entry& a, const Entry& b) {
            //        return a.nOccurances > b.nOccurances;
            //    });

            //    [[maybe_unused]] bool converged = true;
            //    for (int i = 0; i < (int) clusterings.size(); ++i) {
            //        for (int j = 0; j < n; ++j) {
            //            if (mostFrequentClusters[j].nOccurances > (int) (0.90*clusterings.size())) {
            //                if (params.hint[mostFrequentClusters[j].pos] != mostFrequentClusters[j].letter &&
            //                    mostFrequentClusters[j].letter != 27 &&
            //                    mostFrequentClusters[j].letter != 'e' - 'a' + 1) {
            //                    params.hint[mostFrequentClusters[j].pos] = mostFrequentClusters[j].letter;
            //                    converged = false;
            //                    if (i == 0) {
            //                        printf("    - %2d: pos = %d, %c %d\n", i, mostFrequentClusters[j].pos, 'a' + mostFrequentClusters[j].letter - 1, mostFrequentClusters[j].nOccurances);
            //                    }
            //                }
            //            }
            //        }
            //    }

            //    const auto tEnd = std::chrono::high_resolution_clock::now();
            //    printf("[+] Hint refinement took %4.3f seconds\n", toSeconds(tStart, tEnd));
            //}

            //if (converged) {
            //    ++nConverged;
            //    printf("[+] Converged %d times\n", nConverged);
            //    if (nConverged > 0) {
            //        break;
            //    }
            //} else {
            //    nConverged = 0;
            //}

            //for (int i = 0; i < (int) clusterings.size(); ++i) {
            //    for (int j = 0; j < n; ++j) {
            //        for (int k = 0; k < n; ++k) {
            //            if (params.hint[j] != -1 && params.hint[j] == params.hint[k]) {
            //                clusterings[i].clusters[k] = clusterings[i].clusters[j];
            //            }
            //        }
            //    }
            //}
        }
    }

    return 0;
}
