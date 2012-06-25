/*
 * Copyright (c) 2011 Mark D. Hill and David A. Wood
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <cmath>
#include <iostream>
#include <map>

#include "arch/tlb.hh"
#include "arch/utility.hh"
#include "base/chunk_generator.hh"
#include "base/output.hh"
#include "config/the_isa.hh"
#include "cpu/thread_context.hh"
#include "cpu/translation.hh"
#include "debug/GpuTick.hh"
#include "debug/StreamProcessorArray.hh"
#include "debug/StreamProcessorArrayAccess.hh"
#include "debug/StreamProcessorArrayTick.hh"
#include "mem/ruby/system/System.hh"
#include "mem/page_table.hh"
#include "params/StreamProcessorArray.hh"
#include "sp_array.hh"

using namespace TheISA;
using namespace std;

StreamProcessorArray* StreamProcessorArray::singletonPointer = NULL;

StreamProcessorArray::StreamProcessorArray(const Params *p) :
        SimObject(p), _params(p), gpuTickEvent(this, false), streamTickEvent(this, true),
        copyEngine(p->ce), system(p->sys), useGem5Mem(p->useGem5Mem), sharedMemDelay(p->sharedMemDelay),
        launchDelay(p->launchDelay), returnDelay(p->returnDelay), ruby(p->ruby),
        gpuTickConversion(p->gpuTickConv)
{
    streamDelay = 1;
    assert(singletonPointer == NULL);
    singletonPointer = this;

    running = false;

    numShaderCores = 0;

    streamScheduled = false;

    // start our brk point at 2GB. Hopefully this won't clash with what the
    // OS is doing. See arch/x86/process.cc for what it's doing.
    // only used for design point 1 where the CPU and GPU have partitioned mem
    brk_point = 0x8000000;
    // Start giving constant addresses at offset of 0x100 to match GPGPU-Sim
    nextAddr = 0x8000100;

    //
    // Print gpu configuration and stats at exit
    //
    GPUExitCallback* gpuExitCB = new GPUExitCallback(this, p->stats_filename);
    registerExitCallback(gpuExitCB);
}


void StreamProcessorArray::clearStats()
{
    ruby->clearStats();
    clearTick = curTick();
}


int StreamProcessorArray::registerShaderCore(ShaderCore *sc)
{
    // I don't think we need this function. I think it will work the way
    // the ruby system object works.
    shaderCores.push_back(sc);
    return numShaderCores++;
}


void StreamProcessorArray::gpuTick()
{
    DPRINTF(GpuTick, "GPU Tick\n");

    // check if a kernel has completed
    kernelTermInfo term_info = theGPU->finished_kernel();
    if( term_info.grid_uid ) {
        Tick delay = 1;
        Tick curTime = curTick();
        if (curTime - term_info.time < returnDelay*gpuTickConversion ) {
            delay = (Tick)(returnDelay*gpuTickConversion) - (curTime - term_info.time); //delay by whatever is left over
        }

        finished_kernels.push(kernelTermInfo(term_info.grid_uid, curTick()+delay));
        streamRequestTick(1);

        running = false;
    }

    while(!finished_kernels.empty() && finished_kernels.front().time < curTick()) {
        DPRINTF(StreamProcessorArrayTick, "GPU finished a kernel id %d\n", finished_kernels.front().grid_uid);

        DPRINTF(StreamProcessorArray, "GPGPU-sim done! Activating original thread context at %llu.\n", getCurTick());
        streamManager->register_finished_kernel(finished_kernels.front().grid_uid);
        finished_kernels.pop();

        kernelTimes.push_back(getCurTick() - kernelStartTime);

        if (unblockNeeded && streamManager->empty() && finished_kernels.empty()) {
            DPRINTF(StreamProcessorArray, "Stream manager is empty, unblocking\n");
            tc->activate();
            unblockNeeded = false;
        }
    }

    // simulate a clock cycle on the GPU
    if( theGPU->active() ) {
        theGPU->cycle();
    } else {
        if(!finished_kernels.empty()) {
            schedule(gpuTickEvent, finished_kernels.front().time + 1);
        }
    }
    theGPU->deadlock_check();

    if (streamManager->ready() && !streamScheduled) {
        schedule(streamTickEvent, curTick() + streamDelay);
        streamScheduled = true;
    }

}

void StreamProcessorArray::streamTick() {
    DPRINTF(StreamProcessorArrayTick, "Stream Tick\n");

    streamScheduled = false;

    // launch operation on device if one is pending and can be run
    stream_operation op = streamManager->front();
    op.do_operation(theGPU);

    if (streamManager->ready()) {
        schedule(streamTickEvent, curTick() + streamDelay);
        streamScheduled = true;
    }
}


void StreamProcessorArray::unblock()
{
    DPRINTF(StreamProcessorArray, "Unblocking for an event\n");
    assert(tc->status() == ThreadContext::Suspended);
    tc->activate();
}


void StreamProcessorArray::gpuRequestTick(float gpuTicks) {
    Tick gpuWakeupTick = (int)(gpuTicks*gpuTickConversion) + curTick();

    schedule(gpuTickEvent, gpuWakeupTick);
}

void StreamProcessorArray::streamRequestTick(int ticks) {
    if (streamScheduled) {
        DPRINTF(StreamProcessorArrayTick, "Already scheduled a tick, ignoring\n");
        return;
    }
    Tick streamWakeupTick = ticks + curTick();

    schedule(streamTickEvent, streamWakeupTick);
    streamScheduled = true;
}


void StreamProcessorArray::start(LiveProcess *p, ThreadContext *_tc, gpgpu_sim *the_gpu, stream_manager *_stream_manager)
{
    process = p;
    tc = _tc;
    theGPU = the_gpu;
    streamManager = _stream_manager;

    vector<ShaderCore*>::iterator iter;
    for (iter=shaderCores.begin(); iter!=shaderCores.end(); ++iter) {
        (*iter)->initialize(tc);
    }

    copyEngine->initialize(tc, this);

    DPRINTF(StreamProcessorArray, "Starting this stream processor from tc\n");
}


bool StreamProcessorArray::setUnblock()
{
    if (!streamManager->empty()) {
        DPRINTF(StreamProcessorArray, "Suspend request: Need to activate CPU later\n");
        unblockNeeded = true;
        streamManager->print(stdout);
        return true;
    }
    else {
        DPRINTF(StreamProcessorArray, "Suspend request: Already done.\n");
        return false;
    }
}


void StreamProcessorArray::beginRunning(Tick launchTime)
{
    DPRINTF(StreamProcessorArray, "Beginning kernel execution at %llu\n", getCurTick());
    kernelStartTime = getCurTick();
    if (running) {
        panic("Should not already be running if we are starting\n");
    }
    running = true;

    Tick delay = 1;
    Tick curTime = curTick();
    if (curTime - launchTime < launchDelay*gpuTickConversion) {
        delay = (Tick)(launchDelay*gpuTickConversion) - (curTime - launchTime); //delay by whatever is left over
    }

    schedule(gpuTickEvent, curTick()+delay);
}


void StreamProcessorArray::writeFunctional(Addr addr, size_t length, const uint8_t* data)
{
    DPRINTF(StreamProcessorArrayAccess, "Writing to addr 0x%x\n", addr);
    tc->getMemProxy().writeBlob(addr, const_cast<uint8_t*>(data), length);
}

void StreamProcessorArray::readFunctional(Addr addr, size_t length, uint8_t* data)
{
    DPRINTF(StreamProcessorArrayAccess, "Reading from addr 0x%x\n", addr);
    tc->getMemProxy().readBlob(addr, data, length);
}


Addr StreamProcessorArray::allocMemory(size_t length)
{
    // get a new address (NOTE: there's no way it's already allocated)
    // Also NOTE: gpu has its own brk pointer. I guess it's possible that
    // this could collide with the brk_point of the OS. We should look into
    // that.
    Addr addr = nextAddr;

    if (addr + length > brk_point) {
        for (ChunkGenerator gen(brk_point, length, VMPageSize); !gen.done(); gen.next()) {
            process->allocateMem(roundDown(gen.addr(), VMPageSize), VMPageSize);
        }
        brk_point += roundUp(length, VMPageSize);
    }

    nextAddr += length;

    // add the new address to the allocated addresses
    allocatedMemory[addr] = length;

    DPRINTF(StreamProcessorArrayAccess, "Giving the gpu %d bytes at address  0x%x\n", length, addr);

    return addr;
}


void StreamProcessorArray::freeMemory(Addr addr)
{
//     // remove the address from the list of allocated address if
//     // it's there
//     map<Addr,size_t>::iterator iter = allocatedMemory.find(addr);
//     if (iter == allocatedMemory.end()) {
//         return;
//     }
//     size_t size = iter->second;
//     allocatedMemory.erase(iter);
//
//     // We should probably do something like keep a free list... or something
//
//     // un-allocate the pages
//     for (ChunkGenerator gen(addr, size, VMPageSize); !gen.done(); gen.next()) {
//         process->pTable->deallocate(roundDown(gen.addr(), VMPageSize), VMPageSize);
//     }
    fatal("freeMemory is not implemented right now\n");
}

ShaderCore *StreamProcessorArray::getShaderCore(int coreId)
{
    assert(coreId < numShaderCores);
    return shaderCores[coreId];
}


StreamProcessorArray *StreamProcessorArrayParams::create() {
    return new StreamProcessorArray(this);
}

void StreamProcessorArray::gpuPrintStats(std::ostream& out) {
    int i = 0;
    unsigned long long totalKernelTime=0;
    vector<unsigned long long>::iterator it;
    for ( it=kernelTimes.begin() ; it < kernelTimes.end(); it++ ) {
        cout << "kernel[" << i << "] time = " << *it << "\n";
        out << "kernel[" << i << "] time = " << *it << "\n";
        i++;
        totalKernelTime += *it;
    }
    cout << "total kernel time = " << totalKernelTime << "\n";
    out << "total kernel time = " << totalKernelTime << "\n";

    out << "\nMemory System:\n";
    out << "Retires: [";
    vector<ShaderCore*>::iterator iter;
    for (iter=shaderCores.begin(); iter!=shaderCores.end(); iter++) {
        out << (*iter)->numRetry << " ";
    }
    out << "]\n";

    out << "Max outstanding: [";
    for (iter=shaderCores.begin(); iter!=shaderCores.end(); iter++) {
        out << (*iter)->maxOutstanding << " ";
    }
    out << "]\n";
    out << "\n";

    if (clearTick) {
        out << "Stats cleared at tick " << clearTick << "\n";
    }
}

void StreamProcessorArray::memcpy(void *src, void *dst, size_t count, struct CUstream_st *stream) {
    copyEngine->memcpy((Addr)src, (Addr)dst, count, stream);
}


/**
* virtual process function that is invoked when the callback
* queue is executed.
*/
void GPUExitCallback::process()
{
    std::ostream *os = simout.find(stats_filename);
    if (!os) {
        os = simout.create(stats_filename);
    }
    spa_obj->gpuPrintStats(*os);
    *os << std::endl;
}

