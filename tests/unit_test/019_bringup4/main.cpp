/**
 * Copyright (C) 2016-2018 Xilinx, Inc
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#include <getopt.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include "oclHelper.h"

/*
 * Advanced loopback test. The OpenCL kernel returns the data sent to it. Tests the full
 * system. Each workitem copies a 64 byte block of data and 128 workitems ((local size) run
 * in parallel. The full global size is broken into small ranges and Several kernel invocations
   (clEnqueueNDRangeKernel) are made in sequence and data for each invocation is mapped and
   unmapped after the termination of each kernel invocation.
 */


static void checkStatus(cl_int status)
{
    if (status != CL_SUCCESS) {
        throw std::runtime_error(oclErrorCode(status));
    }
}

class KernelHostData {
private:
    char *mSequence1;
    char *mSequence2;
    int mLength;
    int mBlockLength;

private:
    void fillData() {
        static const char repo[] = "ATCG";
        std::srand(std::time(0));
        int i = 0;
        for (; i < mLength - 1; i++) {
            const int index2 = std::rand() % (sizeof(repo) - 1);
            mSequence2[i] = repo[index2];
        }
        mSequence2[i] = '\0';
        std::memset(mSequence1, 0, mLength);
    }

public:
    KernelHostData(int length, int blockLength) : mLength(length), mBlockLength(blockLength) {
        mSequence1 = new char[mLength + 1]; // extra spaces for '\0' at end
        mSequence2 = new char[mLength + 1]; // extra spaces for '\0' at end
        fillData();
    }

    ~KernelHostData() {
        delete [] mSequence1;
        delete [] mSequence2;
    }

    int getLength() const {
        return mLength;
    }

    int getBlockLength() const {
        return mBlockLength;
    }

    char *getSequence1(int blockIndex) const {
        return mSequence1 + blockIndex * mBlockLength;
    }

    char *getSequence2(int blockIndex) const {
        return mSequence2 + blockIndex * mBlockLength;
    }

    int compare(int blockIndex) const {
        return std::memcmp(getSequence1(blockIndex), getSequence2(blockIndex), mBlockLength);
    }
};

class KernelDeviceData {
private:
    cl_mem mSequence1;
    cl_mem mSequence2;


public:
    KernelDeviceData(const KernelHostData &host, unsigned blockIndex, cl_context context) {
        cl_int err = 0;
        mSequence1 = clCreateBuffer(context, CL_MEM_WRITE_ONLY | CL_MEM_USE_HOST_PTR, host.getBlockLength(), host.getSequence1(blockIndex), &err);
        checkStatus(err);

        mSequence2 = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR, host.getBlockLength(), host.getSequence2(blockIndex), &err);
        checkStatus(err);
    }

    ~KernelDeviceData() {
        cl_int err = clReleaseMemObject(mSequence1);
        checkStatus(err);
        err = clReleaseMemObject(mSequence2);
        checkStatus(err);
    }

    cl_mem getSequence1() const {
        return mSequence1;
    }

    cl_mem getSequence2() const {
        return mSequence2;
    }
};

class Timer {
    time_t mTimeStart;
    time_t mTimeEnd;
public:
    Timer() {
        mTimeStart = std::time(0);
        mTimeEnd = mTimeStart;
    }
    double stop() {
        mTimeEnd = std::time(0);
        return std::difftime(mTimeEnd, mTimeStart);
    }
    void reset() {
        mTimeStart = time(0);
        mTimeEnd = mTimeStart;
    }
};

const static struct option long_options[] = {
    {"device",      required_argument, 0, 'd'},
    {"kernel",      required_argument, 0, 'k'},
    {"length",      optional_argument, 0, 'l'},
    {"iteration",   optional_argument, 0, 'i'},
    {"verbose",     no_argument,       0, 'v'},
    {"help",        no_argument,       0, 'h'},
    {0, 0, 0, 0}
};

static void printHelp()
{
    std::cout << "usage: %s <options>\n";
    std::cout << "  -d <device>\n";
    std::cout << "  -k <kernel_file> \n";
    std::cout << "  -i <iteration_count>\n";
    std::cout << "  -l <sequence_length>\n";
    std::cout << "  -v\n";
    std::cout << "  -h\n";
}


int main(int argc, char** argv)
{
    cl_device_type deviceType = CL_DEVICE_TYPE_ACCELERATOR;;
    int option_index = 0;
    std::string kernelFile("kernel.cl");
    int iteration = 5;
    int length = 1600;
    size_t workGroupSize = 128;
    int blockSize = 64;
    bool verbose = false;
    // Commandline
    int c;
    while ((c = getopt_long(argc, argv, "d:k:i:l:v:h", long_options, &option_index)) != -1)
    {
        switch (c)
        {
        case 0:
            if (long_options[option_index].flag != 0)
                break;
        case 'd':
            if (strcmp(optarg, "gpu") == 0)
                deviceType = CL_DEVICE_TYPE_GPU;
            else if (strcmp(optarg, "cpu") == 0)
                deviceType = CL_DEVICE_TYPE_CPU;
            else if (strcmp(optarg, "acc") != 0) {
                std::cout << "Incorrect platform specified\n";
                printHelp();
                return -1;
            }
            break;
        case 'k':
            kernelFile = optarg;
            break;
        case 'i':
            iteration = atoi(optarg);
            break;
        case 'l':
            length = atoi(optarg);
            break;
        case 'h':
            printHelp();
            return 0;
        case 'v':
            verbose = true;
            break;
        default:
            printHelp();
            return 1;
        }
    }


    oclHardware hardware = getOclHardware(deviceType);
    if (!hardware.mQueue) {
        return -1;
    }

    KernelHostData hostData(length * blockSize * workGroupSize, blockSize * workGroupSize);

    oclSoftware software;
    std::memset(&software, 0, sizeof(oclSoftware));
    std::strcpy(software.mKernelName, "loopback");
    std::strcpy(software.mFileName, kernelFile.c_str());
    std::sprintf(software.mCompileOptions, "");

    getOclSoftware(software, hardware);
    double totalDelay = 0.0;

    std::cout << "Block buffer size = " << hostData.getBlockLength() / 1024 << " KB\n";
    std::cout << "Block buffer count = " << length << "\n";
    std::cout << "Total buffer size = " << hostData.getLength() / 1024 << " KB\n";
    const size_t globalSize[1] = {workGroupSize};
    size_t *localSize = 0;

    std::cout << "Global size = " << *globalSize << "\n";
    if (deviceType == CL_DEVICE_TYPE_ACCELERATOR) {
        localSize = &workGroupSize;
        std::cout << "Local size = " << *localSize << "\n";
    }

    try {
        for (int blockIndex = 0; blockIndex < length; blockIndex++) {
//            std::cout << "Block buffer index = " << blockIndex << "\n";
            // Here we start measurings host time for kernel execution
            Timer timer;
            KernelDeviceData deviceData(hostData, blockIndex, hardware.mContext);
            cl_mem seq1 = deviceData.getSequence1();
            cl_mem seq2 = deviceData.getSequence2();

//        std::cout << "Sequence1: " << hostData.getSequence1() << "\n";
//        std::cout << "Sequence2: " << hostData.getSequence2() << "\n";

            cl_int err = clSetKernelArg(software.mKernel, 0, sizeof(cl_mem), &seq1);
            checkStatus(err);

            err = clSetKernelArg(software.mKernel, 1, sizeof(cl_mem), &seq2);
            checkStatus(err);

            err = clEnqueueNDRangeKernel(hardware.mQueue, software.mKernel, 1, 0,
                                         globalSize, localSize, 0, 0, 0);
            checkStatus(err);

            err = clFinish(hardware.mQueue);
            checkStatus(err);

            clEnqueueMapBuffer(hardware.mQueue, deviceData.getSequence1(), CL_TRUE, CL_MAP_READ, 0,
                               hostData.getBlockLength(), 0, 0, 0, &err);

            totalDelay += timer.stop();
            checkStatus(err);
            if (hostData.compare(blockIndex)) {
               std::cout  << "Sequence1: ";
               std::cout.write(hostData.getSequence1(blockIndex),hostData.getBlockLength());
               std::cout << "\n";
               std::cout  << "Sequence2: ";
               std::cout.write(hostData.getSequence2(blockIndex),hostData.getBlockLength());
               std::cout << "\n";
               throw std::runtime_error("Incorrect data from kernel");
            }
            else{
              //std::cout << "Block " << blockIndex << "ok " << std::endl;
            }
        }
        std::cout << "OpenCL kernel time: " << totalDelay << " sec\n";
        release(software);
        release(hardware);
    }
    catch (std::exception const& e)
    {
        std::cout << "Exception: " << e.what() << "\n";
        std::cout << "FAILED TEST\n";
        return 1;
    }
    std::cout << "PASSED TEST\n";
    return 0;
}

