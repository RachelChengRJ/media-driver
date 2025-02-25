/*
* Copyright (c) 2022, Intel Corporation
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included
* in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
* OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
* OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
* ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
* OTHER DEALINGS IN THE SOFTWARE.
*/
//!
//! \file     media_debug_fast_dump_imp.hpp
//!

#pragma once

#include "media_debug_fast_dump.h"

#if USE_MEDIA_DEBUG_TOOL

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class MediaDebugFastDumpImp : public MediaDebugFastDump
{
protected:
    static void Write2File(const std::string &name, const void *data, size_t size)
    {
        std::ofstream ofs(name);
        ofs.write(static_cast<const char *>(data), size);
    }

public:
    MediaDebugFastDumpImp(
        MOS_INTERFACE      &osItf,
        MediaCopyBaseState &mediaCopyItf,
        const Config       *cfg) : m_osItf(osItf),
                             m_mediaCopyItf(mediaCopyItf)
    {
        bool   write2File          = true;
        bool   write2Trace         = false;
        bool   informOnError       = true;
        size_t maxPercentSharedMem = 75;
        size_t maxPercentLocalMem  = 0;
        if (cfg)
        {
            write2File          = cfg->write2File;
            write2Trace         = cfg->write2Trace;
            informOnError       = cfg->informOnError;
            m_allowDataLoss     = cfg->allowDataLoss;
            maxPercentSharedMem = cfg->maxPercentSharedMem;
            maxPercentLocalMem  = cfg->maxPercentLocalMem;
            m_samplingTime      = MS(cfg->samplingTime);
            m_samplingInterval  = MS(cfg->samplingInterval);
        }

        // configure sampling mode
        {
            if (m_samplingTime + m_samplingInterval == MS(0))
            {
                m_2CacheTask = [] { return true; };
            }
            else
            {
                m_2CacheTask = [this] {
                    auto elapsed = std::chrono::duration_cast<MS>(Clock::now() - m_startTP);
                    return (elapsed % (m_samplingTime + m_samplingInterval)) <= m_samplingTime;
                };
            }
        }

        // configure allocator
        {
            m_memMng1st.policy = MOS_MEMPOOL_SYSTEMMEMORY;
            m_memMng2nd.policy = MOS_MEMPOOL_VIDEOMEMORY;

            auto adapter = MosInterface::GetAdapterInfo(m_osItf.osStreamState);
            if (adapter)
            {
                m_memMng1st.cap = static_cast<size_t>(adapter->SystemSharedMemory) / 100 * maxPercentSharedMem;
                m_memMng2nd.cap = static_cast<size_t>(adapter->DedicatedVideoMemory) / 100 * maxPercentLocalMem;
            }
            m_memMng1st.cap = m_memMng1st.cap ? m_memMng1st.cap : -1;

            if (m_memMng2nd.cap > 0)
            {
                m_allocate = [this](ResInfo &resInfo, MOS_RESOURCE &res, size_t resSize) {
                    if (m_memMng1st.usage + resSize > m_memMng1st.cap)
                    {
                        if (m_memMng2nd.usage + resSize > m_memMng2nd.cap)
                        {
                            return false;
                        }
                        resInfo.dwMemType = m_memMng2nd.policy;
                    }
                    else
                    {
                        resInfo.dwMemType = m_memMng1st.policy;
                    }
                    if (m_osItf.pfnAllocateResource(&m_osItf, &resInfo, &res) == MOS_STATUS_SUCCESS)
                    {
                        m_memMng1st.usage += (resInfo.dwMemType == m_memMng1st.policy ? resSize : 0);
                        m_memMng2nd.usage += (resInfo.dwMemType == m_memMng2nd.policy ? resSize : 0);
                        return true;
                    }
                    return false;
                };
            }
            else
            {
                m_allocate = [this](ResInfo &resInfo, MOS_RESOURCE &res, size_t resSize) {
                    if (m_memMng1st.usage + resSize > m_memMng1st.cap)
                    {
                        return false;
                    }
                    resInfo.dwMemType = m_memMng1st.policy;
                    if (m_osItf.pfnAllocateResource(&m_osItf, &resInfo, &res) == MOS_STATUS_SUCCESS)
                    {
                        m_memMng1st.usage += resSize;
                        return true;
                    }
                    return false;
                };
            }
        }

        // configure data write mode
        {
            if (write2File && !write2Trace)
            {
                m_write = [](const std::string &name, const void *data, size_t size) {
                    Write2File(name, data, size);
                };
            }
            else if (!write2File && write2Trace)
            {
                m_write = [](const std::string &name, const void *data, size_t size) {
                    MOS_TraceDataDump(name.c_str(), 0, data, size);
                };
            }
            else if (write2File && write2Trace)
            {
                m_write = [](const std::string &name, const void *data, size_t size) {
                    auto future = std::async(
                        std::launch::async,
                        Write2File,
                        name,
                        data,
                        size);
                    MOS_TraceDataDump(name.c_str(), 0, data, size);
                    future.wait();
                };
            }
            else
            {
                // should not happen
                m_write = [](const std::string &, const void *, size_t) {};
            }

            if (informOnError)
            {
                m_writeError = [this](const std::string &name, const std::string &error) {
                    static const uint8_t dummy = 0;
                    std::thread          w(
                        m_write,
                        name + "." + error,
                        &dummy,
                        sizeof(dummy));
                    if (w.joinable())
                    {
                        w.detach();
                    }
                };
            }
            else
            {
                m_writeError = [](const std::string &, const std::string &) {};
            }
        }

        // launch scheduler thread
        m_scheduler =
#if __cplusplus < 201402L
            std::unique_ptr<std::thread>(new
#else
            std::make_unique<std::thread>(
#endif
                std::thread(
                    [this] {
                        ScheduleTasks();
                    }));

        Res::SetOsInterface(&osItf);
    }

    ~MediaDebugFastDumpImp()
    {
        if (m_scheduler)
        {
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                m_stopScheduler = true;
            }

            if (m_scheduler->joinable())
            {
                m_cond.notify_one();
                m_scheduler->join();
            }
        }
    }

    void AddTask(MOS_RESOURCE &res, std::string &&name, size_t dumpSize, size_t offset)
    {
        if (m_2CacheTask() == false)
        {
            return;
        }

        size_t resSize = 0;

        if (res.pGmmResInfo == nullptr ||
            (resSize = static_cast<size_t>(res.pGmmResInfo->GetSizeMainSurface())) <
                offset + dumpSize)
        {
            m_writeError(
                name,
                res.pGmmResInfo == nullptr ? "get_surface_size_failed" : "incorrect_size_offset");
            return;
        }

        ResInfo resInfo{};

        // fill in resource info
        {
            auto        resType = GetResType(&res);
            MOS_SURFACE details{};

            if (resType != MOS_GFXRES_BUFFER)
            {
                details.Format = Format_Invalid;
            }

            if (m_osItf.pfnGetResourceInfo(&m_osItf, &res, &details) != MOS_STATUS_SUCCESS)
            {
                m_writeError(name, "get_resource_info_failed");
                return;
            }

            resInfo.Type             = resType;
            resInfo.dwWidth          = details.dwWidth;
            resInfo.dwHeight         = details.dwHeight;
            resInfo.TileType         = MOS_TILE_LINEAR;
            resInfo.Format           = details.Format;
            resInfo.Flags.bCacheable = 1;
        }

        // prepare resource pool and resource queue
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            auto                        &resArray = m_resPool[resInfo];

            auto resIt = std::find_if(
                resArray.begin(),
                resArray.end(),
                [](std::remove_reference<decltype(resArray)>::type::const_reference r) {
                    return r->occupied == false;
                });

            if (resIt == resArray.end())
            {
                auto tmpRes = std::make_shared<Res>();
                if (m_allocate(resInfo, tmpRes->res, resSize))
                {
                    resArray.emplace_back(std::move(tmpRes));
                    --(resIt = resArray.end());
                }
                else if (!m_allowDataLoss && !resArray.empty())
                {
                    m_cond.wait(
                        lk,
                        [&] {
                            resIt = std::find_if(
                                resArray.begin(),
                                resArray.end(),
                                [](std::remove_reference<decltype(resArray)>::type::const_reference r) {
                                    return r->occupied == false;
                                });
                            return resIt != resArray.end();
                        });
                }
                else
                {
                    m_writeError(name, "discarded");
                    return;
                }
            }

            if (m_mediaCopyItf.SurfaceCopy(&res, &(*resIt)->res, MCPY_METHOD_PERFORMANCE) != MOS_STATUS_SUCCESS)
            {
                m_writeError(name, "surface_copy_failed");
                return;
            }

            (*resIt)->size     = (dumpSize == 0) ? resSize - offset : dumpSize;
            (*resIt)->offset   = offset;
            (*resIt)->name     = std::move(name);
            (*resIt)->occupied = true;
            m_resQueue.emplace(*resIt);
        }

        m_cond.notify_one();
    }

protected:
    using Clock   = std::chrono::system_clock;
    using MS      = std::chrono::duration<size_t, std::milli>;
    using ResInfo = MOS_ALLOC_GFXRES_PARAMS;

    struct ResInfoCmp
    {
        bool operator()(const ResInfo &a, const ResInfo &b) const
        {
            return a.Type < b.Type ? true : a.dwWidth < b.dwWidth ? true
                                        : a.dwHeight < b.dwHeight ? true
                                        : a.Format < b.Format     ? true
                                                                  : false;
        }
    };

    struct Res
    {
    public:
        static void SetOsInterface(PMOS_INTERFACE itf)
        {
            osItf = itf;
        }

    private:
        static PMOS_INTERFACE osItf;

    public:
        ~Res()
        {
            if (Mos_ResourceIsNull(&res) == false)
            {
                osItf->pfnFreeResource(osItf, &res);
            }
        }

    public:
        bool         occupied = false;
        MOS_RESOURCE res      = {};
        size_t       size     = 0;
        size_t       offset   = 0;
        std::string  name;
    };

    struct MemMng
    {
        Mos_MemPool policy = MOS_MEMPOOL_VIDEOMEMORY;
        size_t      cap    = 0;
        size_t      usage  = 0;
    };

protected:
    void ScheduleTasks()
    {
        std::future<void> future;

        while (true)
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cond.wait(
                lk,
                [this] {
                    return (m_ready4Dump && !m_resQueue.empty()) || m_stopScheduler;
                });

            if (m_stopScheduler)
            {
                break;
            }

            if (m_ready4Dump && !m_resQueue.empty())
            {
                auto qf      = m_resQueue.front();
                m_ready4Dump = false;
                lk.unlock();

                future = std::async(
                    std::launch::async,
                    [this, qf] {
                        DoDump(qf);
                        {
                            std::lock_guard<std::mutex> lk(m_mutex);
                            m_resQueue.front()->occupied = false;
                            m_resQueue.pop();
                            m_ready4Dump = true;
                        }
                        m_cond.notify_all();
                    });
            }
        }

        if (future.valid())
        {
            future.wait();
        }

        std::lock_guard<std::mutex> lk(m_mutex);

        while (!m_resQueue.empty())
        {
            DoDump(m_resQueue.front());
            m_resQueue.front()->occupied = false;
            m_resQueue.pop();
        }
    }

    void DoDump(std::shared_ptr<Res> res) const
    {
        MOS_LOCK_PARAMS lockFlags{};
        lockFlags.ReadOnly     = 1;
        lockFlags.TiledAsTiled = 1;

        auto data = static_cast<const uint8_t *>(
            m_osItf.pfnLockResource(&m_osItf, &res->res, &lockFlags));

        if (data)
        {
            m_write(res->name, data + res->offset, res->size);
            m_osItf.pfnUnlockResource(&m_osItf, &res->res);
        }
        else
        {
            m_writeError(res->name, "lock_failed");
        }
    }

protected:
    // global configurations
    bool   m_allowDataLoss = true;
    MemMng m_memMng1st;
    MemMng m_memMng2nd;
    MS     m_samplingTime{0};
    MS     m_samplingInterval{0};

    const std::chrono::time_point<Clock>
        m_startTP = Clock::now();

    std::unique_ptr<
        std::thread>
        m_scheduler;

    std::map<
        ResInfo,
        std::vector<std::shared_ptr<Res>>,
        ResInfoCmp>
        m_resPool;  // synchronization needed

    std::queue<
        std::shared_ptr<Res>>
        m_resQueue;  // synchronization needed

    std::function<
        bool()>
        m_2CacheTask;

    std::function<
        bool(ResInfo &, MOS_RESOURCE &, size_t)>
        m_allocate;

    std::function<
        void(const std::string &, const void *, size_t)>
        m_write;

    std::function<
        void(const std::string &, const std::string &)>
        m_writeError;

    // threads intercommunication flags, synchronization needed
    bool m_ready4Dump    = true;
    bool m_stopScheduler = false;

    std::mutex              m_mutex;
    std::condition_variable m_cond;

    MOS_INTERFACE      &m_osItf;
    MediaCopyBaseState &m_mediaCopyItf;

    MEDIA_CLASS_DEFINE_END(MediaDebugFastDumpImp)
};

PMOS_INTERFACE MediaDebugFastDumpImp::Res::osItf = nullptr;

#endif  // USE_MEDIA_DEBUG_TOOL
