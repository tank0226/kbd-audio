/*! \file keytap3-app.cpp
 *  \brief keytap3.cpp and key-detector.cpp combined in single app
 *
 *  Used to create the keytap3 web-page
 *
 *  \author Georgi Gerganov
 */

#include "build-vars.h"
#include "common.h"
#include "constants.h"
#include "subbreak3.h"
#include "audio-logger.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>
#include <sstream>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/bind.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

using TSampleInput          = TSampleF;
using TSample               = TSampleI16;
using TWaveform             = TWaveformI16;
using TWaveformView         = TWaveformViewI16;
using TKeyPressData         = TKeyPressDataI16;
using TKeyPressCollection   = TKeyPressCollectionI16;

struct StateIdle {
};

struct StateRecording {
    int captureId = 0;
    int nChannels = 1;
    int filterId = EAudioFilter::FirstOrderHighPass;
    int freqCutoff_Hz = kFreqCutoff_Hz;

    int nKeysToCapture = 100;

    int nKeysHave = 0;
    bool isStarted = false;
    std::atomic_bool doRecord = false;
    std::atomic_bool doneRecording = false;
    size_t totalSize_bytes = 0;

    std::string pathOutput = "record.kbd";

    std::mutex mutex;

    TWaveformF waveformF;
    TWaveformF waveformFWork;
    TWaveformI16 waveformI16;
    TKeyPressCollectionT<TSampleI16> keyPresses;

    AudioLogger audioLogger;
    AudioLogger::Callback cbAudio;

    bool init() {
        if (isStarted && !doneRecording) {
            return false;
        }

        nKeysHave = 0;
        isStarted = true;
        doRecord = true;
        doneRecording = false;
        totalSize_bytes = 0;

        waveformF.clear();
        waveformI16.clear();
        keyPresses.clear();

        AudioLogger::Callback cbAudio = [&](const auto & frames) {
            const auto tStart = std::chrono::high_resolution_clock::now();

            {
                std::lock_guard lock(mutex);
                for (auto & frame : frames) {
                    waveformF.insert(waveformF.end(), frame.begin(), frame.end());
                }

                if (nKeysToCapture <= nKeysHave && !doneRecording) {
                    doneRecording = true;
                    printf("\n[+] Done recording\n");
                } else {
                    doRecord = true;
                }
            }

            const auto tEnd = std::chrono::high_resolution_clock::now();
            const auto tDiff = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - tStart).count();
            if (tDiff > 3) {
                printf("[!] Audio callback took %ld ms\n", (long) tDiff);
            }
        };

        AudioLogger::Parameters parameters;
        parameters.callback = std::move(cbAudio);
        parameters.captureId = captureId;
        parameters.nChannels = nChannels;
        parameters.sampleRate = kSampleRate;
        parameters.filter = EAudioFilter::None;
        parameters.freqCutoff_Hz = freqCutoff_Hz;

        if (audioLogger.install(std::move(parameters)) == false) {
            fprintf(stderr, "Failed to install audio logger\n");
            return false;
        }

        return true;
    }

    void update() {
        if (!isStarted) return;

        if (doRecord) {
            doRecord = false;
            audioLogger.record(1.0f, 0);
        }
    }

    void updateWorker(std::string & dataOutput, float tElapsed) {
        {
            std::lock_guard lock(mutex);
            waveformFWork = waveformF;
        }
        {
            // apply default filtering, because keypress detection without it is impossible
            auto waveformFFiltered = waveformFWork;
            ::filter(waveformFFiltered, EAudioFilter::FirstOrderHighPass, kFreqCutoff_Hz, kSampleRate);

            if (convert(waveformFFiltered, waveformI16) == false) {
                printf("Conversion failed\n");
            }
        }

        TWaveformI16 waveformMax;
        TWaveformI16 waveformThreshold;
        if (findKeyPresses(getView(waveformI16, 0), keyPresses, waveformThreshold, waveformMax, 8.0, 512, 2*1024, true) == false) {
            printf("Failed to detect keypresses\n");
        }

        {
            std::lock_guard lock(mutex);

            if (nKeysHave < (int) keyPresses.size()) {
                nKeysHave = keyPresses.size();

                const float cpm = 60.0f*(nKeysHave/tElapsed);
                dataOutput = "recording " + std::to_string(nKeysHave) + " " + std::to_string(cpm);

                printf("    Detected %d keys. %d left. Average typing speed: %5.2f cpm\n", nKeysHave, nKeysToCapture - nKeysHave, cpm);
            }

            if (tElapsed > 2*60.0f) {
                printf("[!] Recording limit reached\n");
                doneRecording = true;
            }
        }
    }
};

struct StateDecoding {
    std::string pathData = "./data";
    TWaveform waveformInput;
    Cipher::TFreqMap freqMap6;
};

struct State {
    enum EState {
        Loading,
        Idle,
        Recording,
        Decoding,
    };

    EState state = Loading;

    std::string dataOutput;

    StateIdle      idle;
    StateRecording recording;
    StateDecoding  decoding;

    std::thread worker;
    std::atomic_bool workDone = false;

    void init() {
    }

    void deinit() {
    }
};

namespace {

//
// App Interface
//

struct AppInterface {
    std::function<bool()>                     doInit;
    std::function<void(const std::string & )> setData;
    std::function<std::string()>              getData;

    std::function<bool()> mainLoop;

    bool init(State & state);
} g_appInterface;

#ifdef __EMSCRIPTEN__

// need this wrapper of the main loop for the emscripten_set_main_loop_arg() call
void mainLoop(void *) {
    g_appInterface.mainLoop();
}

//
// JS interface
//

// These functions are used to pass data back and forth between the JS and the C++ code
// using the app interface

EMSCRIPTEN_BINDINGS(ggweb) {
    emscripten::function("doInit",        emscripten::optional_override([]() -> int                   { return g_appInterface.doInit(); }));
    emscripten::function("setData",       emscripten::optional_override([](const std::string & input) { g_appInterface.setData(input); }));
    emscripten::function("getData",       emscripten::optional_override([]() -> std::string           { return g_appInterface.getData(); }));
}

#endif

bool AppInterface::init(State & state) {
    doInit = [&]() {
        state.init();

        return true;
    };

    setData = [&](const std::string & data) {
        printf("Received some data from the JS layer: %s\n", data.c_str());

        std::stringstream ss(data);

        std::string cmd;
        ss >> cmd;

        if (cmd == "start") {
            if (state.state == State::Idle) {
                int nKeysToCapture = 0;
                if (ss >> nKeysToCapture) {
                    state.recording.nKeysToCapture = nKeysToCapture;
                } else {
                    printf("Failed to parse nKeysToCapture. Using: %d\n", state.recording.nKeysToCapture);
                }

                printf("Starting recording. nKeysToCapture: %d\n", state.recording.nKeysToCapture);
                state.state = State::Recording;
                state.recording.init();
            }
        } else if (cmd == "stop") {
            state.state = State::Idle;
        } else {
            printf("Unknown cmd: %s\n", cmd.c_str());
        }
    };

    getData = [&]() {
        const auto res = state.dataOutput;
        state.dataOutput.clear();
        return res;
    };

    mainLoop = [&]() {
        switch (state.state) {
            case State::Loading:
                {
                    if (state.worker.joinable() == false) {
                        state.dataOutput = "loading";
                        state.worker = std::thread([&]() {
                            const auto tStart = std::chrono::high_resolution_clock::now();

                            const auto filename = state.decoding.pathData + "/./ggwords-6-gram.dat.binary";
                            printf("[+] Loading n-grams from '%s'\n", filename.c_str());

                            if (Cipher::loadFreqMapBinary(filename.c_str(), state.decoding.freqMap6) == false) {
                                printf("[E] Failed to load n-grams\n");
                                return;
                            }

                            const auto tEnd = std::chrono::high_resolution_clock::now();

                            printf("[+] Loading took %4.3f seconds\n", toSeconds(tStart, tEnd));
                            state.workDone = true;
                        });
                    }

                    if (state.workDone) {
                        state.worker.join();
                        state.state = State::Idle;
                        state.dataOutput = "loaded";
                    }
                } break;
            case State::Idle:
                {
                } break;
            case State::Recording:
                {
                    state.recording.update();

                    if (state.worker.joinable() == false) {
                        state.worker = std::thread([&]() {
                            const auto tStart = std::chrono::high_resolution_clock::now();

                            while (state.recording.doneRecording == false) {
                                const auto tNow = std::chrono::high_resolution_clock::now();
                                const float tElapsed = std::chrono::duration<float>(tNow - tStart).count();

                                state.recording.updateWorker(state.dataOutput, tElapsed);

                                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                            }

                            const auto tEnd = std::chrono::high_resolution_clock::now();

                            printf("[+] Recording took %4.3f seconds\n", toSeconds(tStart, tEnd));
                        });
                    }

                    if (state.recording.doneRecording) {
                        state.recording.audioLogger.terminate();
                        state.worker.join();

                        std::ofstream fout(state.recording.pathOutput, std::ios::binary);
                        if (fout.good() == false) {
                            fprintf(stderr, "Failed to open file '%s'\n", state.recording.pathOutput.c_str());
                            return false;
                        }

                        state.recording.totalSize_bytes = sizeof(state.recording.waveformF[0])*state.recording.waveformF.size();
                        fout.write((char *)(state.recording.waveformF.data()), sizeof(state.recording.waveformF[0])*state.recording.waveformF.size());
                        fout.close();

                        printf("Total data saved: %g MB\n", ((float)(state.recording.totalSize_bytes)/1024.0f/1024.0f));
                        state.dataOutput = "decoding";

                        state.decoding.waveformInput = state.recording.waveformI16;
                        state.state = State::Decoding;
                        printf("Starting decoding\n");
                    }
                } break;
            case State::Decoding:
                {
                    if (state.worker.joinable() == false) {
                        state.workDone = false;
                        state.worker = std::thread([&]() {
                            TKeyPressCollection keyPresses;
                            {
                                const auto tStart = std::chrono::high_resolution_clock::now();

                                printf("[+] Searching for key presses\n");

                                TWaveform waveformMax;
                                TWaveform waveformThreshold;
                                if (findKeyPresses(getView(state.decoding.waveformInput, 0), keyPresses, waveformThreshold, waveformMax, 8.0, 512, 2*1024, true) == false) {
                                    printf("Failed to detect keypresses\n");
                                    return;
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

                                if (calculateSimilartyMap(3*256, 3*32, 3*256 - 128, keyPresses, similarityMap) == false) {
                                    printf("Failed to calculate similariy map\n");
                                    return;
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
                            {
                                Cipher::Processor processor;

                                Cipher::TParameters params;
                                params.maxClusters = 29;
                                params.wEnglishFreq = 20.0;
                                params.nHypothesesToKeep = std::max(100, 2100 - 10*std::min(200, std::max(0, ((int) keyPresses.size() - 100))));
                                processor.init(params, state.decoding.freqMap6, similarityMap);

                                printf("[+] Attempting to recover the text from the recording. nHypothesesToKeep = %d\n", params.nHypothesesToKeep);

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
                                        processor.init(params, state.decoding.freqMap6, similarityMap);
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
                                            Cipher::beamSearch(params, state.decoding.freqMap6, clusterings[i]);
                                            printf("%8.3f %8.3f ", clusterings[i].p, clusterings[i].pClusters);
                                            Cipher::printDecoded(clusterings[i].clusters, clusterings[i].clMap, params.hint);
                                            Cipher::refineNearby(params, state.decoding.freqMap6, clusterings[i]);
                                        }
                                    }

                                    break;
                                }
                            }

                            printf("[+] Done\n");
                            state.dataOutput = "done";
                            state.workDone = true;
                        });
                    }

                    if (state.workDone) {
                        state.worker.join();
                        state.state = State::Idle;
                    }
                } break;
            default:
                break;
        };

        return true;
    };

    return true;
}

}


int main(int argc, char ** argv) {
    printf("Build info: %s, %s, %s\n", kGIT_DATE, kGIT_SHA1, kGIT_COMMIT_SUBJECT);
    printf("Usage: %s record.kbd n-gram-dir nkeys [-cN] [-CN] [-FN] [-fN]\n", argv[0]);
    printf("    -cN - select capture device N\n");
    printf("    -CN - number N of capture channels N\n");
    printf("    -FN - select filter type, (0 - none, 1 - first order high-pass, 2 - second order high-pass)\n");
    printf("    -fN - cutoff frequency in Hz\n");

    if (argc < 4) {
        return -1;
    }

    const auto argm = parseCmdArguments(argc, argv);
    const int captureId     = argm.count("c") == 0 ? 0 : std::stoi(argm.at("c"));
    const int nChannels     = argm.count("C") == 0 ? 0 : std::stoi(argm.at("C"));
    const int filterId      = argm.count("F") == 0 ? EAudioFilter::FirstOrderHighPass : std::stoi(argm.at("F"));
    const int freqCutoff_Hz = argm.count("f") == 0 ? kFreqCutoff_Hz : std::stoi(argm.at("f"));

    const int nKeysToCapture = atoi(argv[3]);

    State state;

    state.recording.captureId = captureId;
    state.recording.nChannels = nChannels;
    state.recording.filterId = filterId;
    state.recording.freqCutoff_Hz = freqCutoff_Hz;
    state.recording.nKeysToCapture = nKeysToCapture;
    state.recording.pathOutput = argv[1];

    state.decoding.pathData = argv[2];

    // initialize the application interface
    if (g_appInterface.init(state) == false) {
        fprintf(stderr, "Error: failed to initialize app interface.\n");
        return -4;
    }

#ifdef __EMSCRIPTEN__
    // main - emscripten
    {
        emscripten_set_main_loop_arg(mainLoop, NULL, 0, true);
    }
#else
    // main - native
    {
        if (g_appInterface.doInit() == false) {
            printf("Error: failed to initialize\n");
            return -5;
        }

        int nLoops = 500;

        // main loop
        while (true) {
            if (g_appInterface.mainLoop() == false) {
                printf("Main loop exited\n");
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            ++nLoops;
            if (nLoops % 1000 == 0) {
                printf("State: %d\n", (int) state.state);
                g_appInterface.setData("start");
            }
        }

        // cleanup
        {
            state.deinit();
        }
    }
#endif

    return 0;
}
