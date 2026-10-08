#ifndef EVO_COVER_ART_SERVICE_HPP
#define EVO_COVER_ART_SERVICE_HPP

#include "evo/interfaces/ICoverArtService.hpp"
#include <pthread.h>
#include <deque>
#include <set>
#include <string>
#include <vector>

namespace evo {

class CoverArtService : public ICoverArtService {
public:
    CoverArtService();

    std::string resolveSidecarPath(const std::string& mediaPath, bool isDirectory) override;
    const uint32_t* peekCoverArt(const std::string& mediaPath) const override;
    bool hasTriedCoverArt(const std::string& mediaPath) const override;
    const uint32_t* getCoverArt(const std::string& mediaPath, bool isDirectory) override;

    void ensureHeroArt(const std::string& mediaPath) override;
    const uint32_t* getHeroArtPixels() const override { return m_heroArtValid ? m_heroArtPixels.data() : nullptr; }
    bool isHeroArtValid() const override { return m_heroArtValid; }
    const std::string& getHeroArtPath() const override { return m_heroArtPath; }

    void ensureBrowserPreview(const std::string& mediaPath, bool isDirectory) override;
    const uint32_t* getBrowserPreviewPixels() const override;
    bool isBrowserPreviewValid() const override { return m_browserPreviewValid; }

    bool extractVideoFrame(const std::string& videoPath, uint32_t* outPixels, int targetWidth, int targetHeight) override;
    void boxFilterScaleRgba(const uint8_t* sourceRgba, int srcWidth, int srcHeight,
                            uint32_t* destBgra, int destWidth, int destHeight) override;

    void clearCache() override;

    void requestCoverArt(const std::string& mediaPath, bool isDirectory) override;
    void requestBrowserPreview(const std::string& mediaPath, bool isDirectory) override;
    void pumpAsync() override;
    void quiesce() override;

    ~CoverArtService() override;

private:
    struct Job {
        bool preview = false;
        std::string path;
        bool isDir = false;
        unsigned generation = 0;
    };
    struct Result {
        Job job;
        bool ok = false;
        std::vector<uint32_t> pixels;
    };

    bool loadArtInto(const std::string& mediaPath, bool isDirectory,
                     uint32_t* out, int w, int h);
    void startWorker();
    void workerMain();
    static void* workerEntry(void* self);

    pthread_t m_worker;
    bool m_workerStarted = false;
    pthread_mutex_t m_jobMutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t m_jobCond = PTHREAD_COND_INITIALIZER;
    pthread_cond_t m_idleCond = PTHREAD_COND_INITIALIZER;
    bool m_stop = false;
    bool m_busy = false;
    unsigned m_generation = 0;
    std::deque<Job> m_posterJobs;           /* guarded by m_jobMutex */
    bool m_hasPreviewJob = false;
    Job m_previewJob;
    std::deque<Result> m_results;
    std::set<std::string> m_pendingPosters; /* UI thread only */
    bool m_previewPending = false;          /* UI thread only */

    struct CacheEntry {
        std::string pathKey;
        std::vector<uint32_t> pixels;
        bool valid = false;
        bool tried = false;
    };

    CacheEntry* findOrAllocateSlot(const std::string& key);
    const CacheEntry* findSlot(const std::string& key) const;
    bool isVideoFile(const std::string& path) const;

    std::vector<CacheEntry> m_cache;
    int m_accessClock = 0;

    std::vector<uint32_t> m_heroArtPixels;
    std::string m_heroArtPath;
    bool m_heroArtValid = false;

    std::vector<uint32_t> m_browserPreviewPixels;
    std::string m_browserPreviewPath;
    bool m_browserPreviewValid = false;
    bool m_browserPreviewFailed = false;
};

} // namespace evo

#endif // EVO_COVER_ART_SERVICE_HPP
