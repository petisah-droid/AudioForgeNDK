#include <jni.h>
#include <aaudio/AAudio.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaMuxer.h>
#include <media/NdkMediaFormat.h>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <mutex>
#include <atomic>
#include <fstream>
#include <cstring>
#include <complex>
#include <fcntl.h>
#include <unistd.h>

static constexpr float PI_F = 3.14159265358979323846f;

struct AudioBuffer {
    std::vector<float> left, right;
    int sampleRate = 48000;
    size_t frameCount() const { return left.size(); }
    double durationMs() const { return sampleRate > 0 ? (left.size() * 1000.0) / sampleRate : 0.0; }
};

static AudioBuffer gBuffer;
static std::mutex gMutex;
static std::atomic<bool> gIsPlaying{false};
static std::atomic<size_t> gPlayheadFrame{0}, gLoopStartFrame{0}, gLoopEndFrame{0};
static AAudioStream* gStream = nullptr;

static void resampleChannel(const std::vector<float>& in, int inRate, std::vector<float>& out, int outRate) {
    if (in.empty() || inRate <= 0 || outRate <= 0) { out.clear(); return; }
    if (inRate == outRate) { out = in; return; }
    double ratio = static_cast<double>(inRate) / outRate;
    size_t outLen = static_cast<size_t>(in.size() / ratio);
    out.resize(outLen);
    for (size_t i = 0; i < outLen; ++i) {
        double pos = i * ratio;
        size_t i0 = static_cast<size_t>(pos);
        size_t i1 = std::min(i0 + 1, in.size() - 1);
        float f = static_cast<float>(pos - i0);
        out[i] = in[i0] * (1.0f - f) + in[i1] * f;
    }
}

static bool decodeMediaFile(const std::string& path, AudioBuffer& outBuf) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    off_t sz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    AMediaExtractor* ex = AMediaExtractor_new();
    media_status_t err = AMediaExtractor_setDataSourceFd(ex, fd, 0, sz);
    close(fd);
    if (err != AMEDIA_OK) { AMediaExtractor_delete(ex); return false; }

    int tracks = AMediaExtractor_getTrackCount(ex), aIdx = -1;
    AMediaFormat* fmt = nullptr;
    const char* mime = nullptr;
    for (int i = 0; i < tracks; ++i) {
        AMediaFormat* f = AMediaExtractor_getTrackFormat(ex, i);
        const char* m = nullptr;
        if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &m) && m && strncmp(m, "audio/", 6) == 0) {
            aIdx = i; fmt = f; mime = m; break;
        }
        AMediaFormat_delete(f);
    }
    if (aIdx < 0) { AMediaExtractor_delete(ex); return false; }

    AMediaExtractor_selectTrack(ex, aIdx);
    AMediaCodec* codec = AMediaCodec_createDecoderByType(mime);
    if (!codec || AMediaCodec_configure(codec, fmt, nullptr, nullptr, 0) != AMEDIA_OK) {
        if (codec) AMediaCodec_delete(codec);
        AMediaFormat_delete(fmt); AMediaExtractor_delete(ex); return false;
    }
    AMediaCodec_start(codec);

    int sr = 48000, ch = 2;
    AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_SAMPLE_RATE, &sr);
    AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &ch);
    std::vector<float> rawL, rawR;
    bool inEOS = false, outEOS = false;
    AMediaCodecBufferInfo info;

    while (!outEOS) {
        if (!inEOS) {
            ssize_t idx = AMediaCodec_dequeueInputBuffer(codec, 5000);
            if (idx >= 0) {
                size_t cap = 0;
                uint8_t* buf = AMediaCodec_getInputBuffer(codec, idx, &cap);
                ssize_t n = AMediaExtractor_readSampleData(ex, buf, cap);
                if (n < 0) { AMediaCodec_queueInputBuffer(codec, idx, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM); inEOS = true; }
                else { AMediaCodec_queueInputBuffer(codec, idx, 0, n, AMediaExtractor_getSampleTime(ex), 0); AMediaExtractor_advance(ex); }
            }
        }
        ssize_t oIdx = AMediaCodec_dequeueOutputBuffer(codec, &info, 5000);
        if (oIdx >= 0) {
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) outEOS = true;
            size_t oSz = 0;
            uint8_t* oData = AMediaCodec_getOutputBuffer(codec, oIdx, &oSz);
            if (oData && info.size > 0) {
                const int16_t* pcm = reinterpret_cast<const int16_t*>(oData + info.offset);
                size_t smp = info.size / sizeof(int16_t);
                if (ch == 1) {
                    for (size_t i = 0; i < smp; ++i) { float v = pcm[i] / 32768.0f; rawL.push_back(v); rawR.push_back(v); }
                } else {
                    for (size_t f = 0; f < smp / ch; ++f) { rawL.push_back(pcm[f * ch] / 32768.0f); rawR.push_back(pcm[f * ch + 1] / 32768.0f); }
                }
            }
            AMediaCodec_releaseOutputBuffer(codec, oIdx, false);
        } else if (oIdx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* nf = AMediaCodec_getOutputFormat(codec);
            AMediaFormat_getInt32(nf, AMEDIAFORMAT_KEY_SAMPLE_RATE, &sr);
            AMediaFormat_getInt32(nf, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &ch);
            AMediaFormat_delete(nf);
        }
    }
    AMediaCodec_stop(codec); AMediaCodec_delete(codec); AMediaFormat_delete(fmt); AMediaExtractor_delete(ex);
    if (rawL.empty()) return false;
    outBuf.sampleRate = 48000;
    resampleChannel(rawL, sr, outBuf.left, 48000);
    resampleChannel(rawR, sr, outBuf.right, 48000);
    return true;
}

static aaudio_data_callback_result_t aaudioCallback(AAudioStream*, void*, void* audioData, int32_t numFrames) {
    float* out = static_cast<float*>(audioData);
    if (!gIsPlaying.load()) { std::memset(out, 0, sizeof(float) * numFrames * 2); return AAUDIO_CALLBACK_RESULT_CONTINUE; }
    std::unique_lock<std::mutex> lock(gMutex, std::try_to_lock);
    if (!lock.owns_lock() || gBuffer.left.empty()) { std::memset(out, 0, sizeof(float) * numFrames * 2); return AAUDIO_CALLBACK_RESULT_CONTINUE; }
    size_t pos = gPlayheadFrame.load(), endF = gLoopEndFrame.load();
    if (endF == 0 || endF > gBuffer.frameCount()) endF = gBuffer.frameCount();
    size_t startF = std::min(gLoopStartFrame.load(), endF);
    for (int32_t i = 0; i < numFrames; ++i) {
        if (pos >= endF) { gIsPlaying.store(false); pos = startF; out[i*2] = 0.0f; out[i*2+1] = 0.0f; }
        else { out[i*2] = gBuffer.left[pos]; out[i*2+1] = gBuffer.right[pos]; ++pos; }
    }
    gPlayheadFrame.store(pos);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void initAAudio() {
    if (gStream) return;
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(b, 48000);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, aaudioCallback, nullptr);
    if (AAudioStreamBuilder_openStream(b, &gStream) == AAUDIO_OK) AAudioStream_requestStart(gStream);
    AAudioStreamBuilder_delete(b);
}

static size_t findNearestZeroCrossing(const std::vector<float>& ch, size_t target, int sr) {
    if (ch.empty()) return 0;
    target = std::min(target, ch.size() - 1);
    size_t win = static_cast<size_t>(sr * 0.015), lo = target > win ? target - win : 1, hi = std::min(ch.size() - 1, target + win);
    size_t best = target, bestDist = win + 100; float bestAmp = std::fabs(ch[target]);
    for (size_t i = lo; i <= hi; ++i) {
        bool cross = (ch[i-1] <= 0.0f && ch[i] >= 0.0f) || (ch[i-1] >= 0.0f && ch[i] <= 0.0f);
        size_t d = i > target ? i - target : target - i;
        if (cross && d < bestDist) { bestDist = d; best = i; }
        else if (bestDist > win && std::fabs(ch[i]) < bestAmp) { bestAmp = std::fabs(ch[i]); best = i; }
    }
    return best;
}

static void fftInPlace(std::vector<std::complex<float>>& a) {
    size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * PI_F / len; std::complex<float> wlen(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t j = 0; j < len / 2; ++j) {
                std::complex<float> u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v; a[i + j + len / 2] = u - v; w *= wlen;
            }
        }
    }
}

static bool writeAudioFile(const std::string& path, const AudioBuffer& buf, int rate, int format, int bitrateBps, int channelMode) {
    std::vector<float> l, r;
    resampleChannel(buf.left, buf.sampleRate, l, rate);
    resampleChannel(buf.right, buf.sampleRate, r, rate);
    if (l.empty()) return false;
    int ch = channelMode == 1 ? 1 : 2;
    if (channelMode == 2) std::swap(l, r);

    if (format == 3 || format == 4) {
        const char* mime = (format == 3) ? "audio/flac" : "audio/mp4a-latm";
        int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return false;
        AMediaMuxer* mux = AMediaMuxer_new(fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
        close(fd);
        if (!mux) return false;
        AMediaFormat* fmt = AMediaFormat_new();
        AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, mime);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_SAMPLE_RATE, rate);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT, ch);
        if (format == 4) { AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_BIT_RATE, bitrateBps); AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_AAC_PROFILE, 2); }
        AMediaCodec* codec = AMediaCodec_createEncoderByType(mime);
        if (!codec || AMediaCodec_configure(codec, fmt, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE) != AMEDIA_OK) {
            if (codec) AMediaCodec_delete(codec);
            AMediaFormat_delete(fmt); AMediaMuxer_delete(mux); return false;
        }
        AMediaCodec_start(codec);
        size_t total = l.size(), cur = 0; bool inDone = false, outDone = false, started = false; ssize_t trk = -1; AMediaCodecBufferInfo info;
        while (!outDone) {
            if (!inDone) {
                ssize_t idx = AMediaCodec_dequeueInputBuffer(codec, 5000);
                if (idx >= 0) {
                    size_t cap = 0; uint8_t* ib = AMediaCodec_getInputBuffer(codec, idx, &cap);
                    size_t n = std::min(cap / (ch * sizeof(int16_t)), total - cur); int64_t pts = (cur * 1000000LL) / rate;
                    if (n == 0) { AMediaCodec_queueInputBuffer(codec, idx, 0, 0, pts, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM); inDone = true; }
                    else {
                        int16_t* dst = (int16_t*)ib;
                        for (size_t i = 0; i < n; ++i) {
                            float lv = std::clamp(l[cur+i], -1.0f, 1.0f), rv = std::clamp(r[cur+i], -1.0f, 1.0f);
                            if (ch == 1) dst[i] = (int16_t)(0.5f * (lv + rv) * 32767.0f);
                            else { dst[i*2] = (int16_t)(lv * 32767.0f); dst[i*2+1] = (int16_t)(rv * 32767.0f); }
                        }
                        cur += n; AMediaCodec_queueInputBuffer(codec, idx, 0, n * ch * sizeof(int16_t), pts, 0);
                    }
                }
            }
            ssize_t oIdx = AMediaCodec_dequeueOutputBuffer(codec, &info, 5000);
            if (oIdx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                AMediaFormat* of = AMediaCodec_getOutputFormat(codec); trk = AMediaMuxer_addTrack(mux, of); AMediaMuxer_start(mux); started = true; AMediaFormat_delete(of);
            } else if (oIdx >= 0) {
                size_t osz = 0; uint8_t* ob = AMediaCodec_getOutputBuffer(codec, oIdx, &osz);
                if (!(info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) && info.size > 0 && started) AMediaMuxer_writeSampleData(mux, trk, ob, &info);
                if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) outDone = true;
                AMediaCodec_releaseOutputBuffer(codec, oIdx, false);
            }
        }
        AMediaCodec_stop(codec); AMediaCodec_delete(codec); if (started) AMediaMuxer_stop(mux); AMediaMuxer_delete(mux); AMediaFormat_delete(fmt);
        return true;
    }

    int bitDepth = (format == 0) ? 16 : ((format == 1) ? 24 : 32);
    uint32_t frames = l.size(), bytesPerSmp = bitDepth / 8, dataSz = frames * ch * bytesPerSmp, riffSz = 36 + dataSz, byteRate = rate * ch * bytesPerSmp;
    uint16_t align = ch * bytesPerSmp, fmtCode = (format == 2) ? 3 : 1, ch16 = ch, bd16 = bitDepth; uint32_t fmtSz = 16, sr32 = rate;
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    f.write("RIFF", 4); f.write((char*)&riffSz, 4); f.write("WAVEfmt ", 8);
    f.write((char*)&fmtSz, 4); f.write((char*)&fmtCode, 2); f.write((char*)&ch16, 2);
    f.write((char*)&sr32, 4); f.write((char*)&byteRate, 4); f.write((char*)&align, 2);
    f.write((char*)&bd16, 2); f.write("data", 4); f.write((char*)&dataSz, 4);
    for (uint32_t i = 0; i < frames; ++i) {
        float lv = std::clamp(l[i], -1.0f, 1.0f), rv = std::clamp(r[i], -1.0f, 1.0f);
        if (ch == 1) {
            float m = 0.5f * (lv + rv);
            if (format == 2) f.write((char*)&m, 4);
            else if (format == 1) { int32_t v = m * 8388607.0f; uint8_t b[3] = {(uint8_t)v, (uint8_t)(v>>8), (uint8_t)(v>>16)}; f.write((char*)b, 3); }
            else { int16_t v = m * 32767.0f; f.write((char*)&v, 2); }
        } else {
            float p[2] = {lv, rv};
            for (int c = 0; c < 2; ++c) {
                if (format == 2) f.write((char*)&p[c], 4);
                else if (format == 1) { int32_t v = p[c] * 8388607.0f; uint8_t b[3] = {(uint8_t)v, (uint8_t)(v>>8), (uint8_t)(v>>16)}; f.write((char*)b, 3); }
                else { int16_t v = p[c] * 32767.0f; f.write((char*)&v, 2); }
            }
        }
    }
    return true;
}

extern "C" {
JNIEXPORT void JNICALL Java_id_naga_audioforge_MainActivity_nativeInitAudio(JNIEnv*, jobject) { initAAudio(); }
JNIEXPORT jboolean JNICALL Java_id_naga_audioforge_MainActivity_nativeLoadFile(JNIEnv* env, jobject, jstring jp) {
    const char* cp = env->GetStringUTFChars(jp, nullptr); std::string p(cp); env->ReleaseStringUTFChars(jp, cp);
    AudioBuffer t; if (!decodeMediaFile(p, t)) return JNI_FALSE;
    std::lock_guard<std::mutex> lk(gMutex); gIsPlaying.store(false); gBuffer = std::move(t);
    gPlayheadFrame.store(0); gLoopStartFrame.store(0); gLoopEndFrame.store(gBuffer.frameCount()); return JNI_TRUE;
}
JNIEXPORT jboolean JNICALL Java_id_naga_audioforge_MainActivity_nativeAppendMergeFile(JNIEnv* env, jobject, jstring jp, jint xfMs) {
    const char* cp = env->GetStringUTFChars(jp, nullptr); std::string p(cp); env->ReleaseStringUTFChars(jp, cp);
    AudioBuffer c; if (!decodeMediaFile(p, c)) return JNI_FALSE;
    std::lock_guard<std::mutex> lk(gMutex); gIsPlaying.store(false);
    if (gBuffer.left.empty()) gBuffer = std::move(c);
    else {
        size_t xf = std::min({(size_t)((xfMs / 1000.0) * 48000), gBuffer.frameCount() / 2, c.frameCount() / 2});
        if (xf > 0) {
            size_t off = gBuffer.frameCount() - xf;
            for (size_t i = 0; i < xf; ++i) {
                float t = (float)i / xf, go = std::cos(t * 0.5f * PI_F), gi = std::sin(t * 0.5f * PI_F);
                gBuffer.left[off+i] = gBuffer.left[off+i]*go + c.left[i]*gi;
                gBuffer.right[off+i] = gBuffer.right[off+i]*go + c.right[i]*gi;
            }
            gBuffer.left.insert(gBuffer.left.end(), c.left.begin() + xf, c.left.end());
            gBuffer.right.insert(gBuffer.right.end(), c.right.begin() + xf, c.right.end());
        } else {
            gBuffer.left.insert(gBuffer.left.end(), c.left.begin(), c.left.end());
            gBuffer.right.insert(gBuffer.right.end(), c.right.begin(), c.right.end());
        }
    }
    gLoopStartFrame.store(0); gLoopEndFrame.store(gBuffer.frameCount()); return JNI_TRUE;
}
JNIEXPORT jdouble JNICALL Java_id_naga_audioforge_MainActivity_nativeGetDurationMs(JNIEnv*, jobject) { std::lock_guard<std::mutex> lk(gMutex); return gBuffer.durationMs(); }
JNIEXPORT jdouble JNICALL Java_id_naga_audioforge_MainActivity_nativeGetPlayheadMs(JNIEnv*, jobject) { return (gPlayheadFrame.load() * 1000.0) / 48000.0; }
JNIEXPORT void JNICALL Java_id_naga_audioforge_MainActivity_nativeSetPlaybackRange(JNIEnv*, jobject, jdouble s, jdouble e, jboolean play) {
    size_t sf = (s / 1000.0) * 48000, ef = (e / 1000.0) * 48000;
    gLoopStartFrame.store(sf); gLoopEndFrame.store(ef); gPlayheadFrame.store(sf); gIsPlaying.store(play == JNI_TRUE);
}
JNIEXPORT void JNICALL Java_id_naga_audioforge_MainActivity_nativeScrubToMs(JNIEnv*, jobject, jdouble ms, jboolean aud) {
    std::lock_guard<std::mutex> lk(gMutex); if (gBuffer.left.empty()) return;
    size_t f = std::min((size_t)((ms / 1000.0) * 48000), gBuffer.frameCount() - 1); gPlayheadFrame.store(f);
    if (aud == JNI_TRUE) { gLoopStartFrame.store(f); gLoopEndFrame.store(std::min(gBuffer.frameCount(), f + 2400)); gIsPlaying.store(true); }
}
JNIEXPORT void JNICALL Java_id_naga_audioforge_MainActivity_nativeTogglePlay(JNIEnv*, jobject, jboolean play) { gIsPlaying.store(play == JNI_TRUE); }
JNIEXPORT jdouble JNICALL Java_id_naga_audioforge_MainActivity_nativeSnapZeroCrossingMs(JNIEnv*, jobject, jdouble ms) {
    std::lock_guard<std::mutex> lk(gMutex); if (gBuffer.left.empty()) return ms;
    return (findNearestZeroCrossing(gBuffer.left, (ms / 1000.0) * 48000, 48000) * 1000.0) / 48000.0;
}
JNIEXPORT jfloatArray JNICALL Java_id_naga_audioforge_MainActivity_nativeGetWaveformPeaks(JNIEnv* env, jobject, jint bins) {
    std::lock_guard<std::mutex> lk(gMutex); jfloatArray arr = env->NewFloatArray(bins);
    if (gBuffer.left.empty() || bins <= 0) return arr;
    std::vector<float> p(bins, 0.0f); size_t tot = gBuffer.frameCount(); double st = (double)tot / bins;
    for (int bIdx = 0; bIdx < bins; ++bIdx) {
        size_t s = bIdx * st, e = std::min(tot, (size_t)((bIdx+1)*st) + 1); float mx = 0.0f;
        for (size_t i = s; i < e; i += std::max<size_t>(1, (e-s)/64)) mx = std::max(mx, std::max(std::fabs(gBuffer.left[i]), std::fabs(gBuffer.right[i])));
        p[bIdx] = mx;
    }
    env->SetFloatArrayRegion(arr, 0, bins, p.data()); return arr;
}
JNIEXPORT jfloatArray JNICALL Java_id_naga_audioforge_MainActivity_nativeGetZoomSamples(JNIEnv* env, jobject, jdouble cMs, jdouble wMs, jint pts) {
    std::lock_guard<std::mutex> lk(gMutex); jfloatArray arr = env->NewFloatArray(pts);
    if (gBuffer.left.empty() || pts <= 0) return arr;
    std::vector<float> out(pts, 0.0f);
    size_t sf = std::max(0.0, cMs - wMs*0.5) / 1000.0 * 48000;
    size_t ef = std::min(gBuffer.frameCount(), (size_t)(std::min(gBuffer.durationMs(), cMs + wMs*0.5) / 1000.0 * 48000));
    if (ef <= sf) return arr;
    double st = (double)(ef - sf) / pts;
    for (int i = 0; i < pts; ++i) { size_t idx = std::min(gBuffer.frameCount()-1, sf + (size_t)(i*st)); out[i] = 0.5f*(gBuffer.left[idx]+gBuffer.right[idx]); }
    env->SetFloatArrayRegion(arr, 0, pts, out.data()); return arr;
}
JNIEXPORT jboolean JNICALL Java_id_naga_audioforge_MainActivity_nativeTrimKeepRange(JNIEnv*, jobject, jdouble inMs, jdouble outMs) {
    std::lock_guard<std::mutex> lk(gMutex); if (gBuffer.left.empty() || outMs <= inMs + 5.0) return JNI_FALSE; gIsPlaying.store(false);
    size_t s = findNearestZeroCrossing(gBuffer.left, (inMs/1000.0)*48000, 48000);
    size_t e = std::min(gBuffer.frameCount(), findNearestZeroCrossing(gBuffer.left, (outMs/1000.0)*48000, 48000));
    if (e <= s + 240) return JNI_FALSE;
    std::vector<float> nl(gBuffer.left.begin()+s, gBuffer.left.begin()+e), nr(gBuffer.right.begin()+s, gBuffer.right.begin()+e);
    size_t fd = std::min<size_t>(240, nl.size()/4);
    for (size_t i = 0; i < fd; ++i) { float g = (float)i/fd; nl[i]*=g; nr[i]*=g; nl[nl.size()-1-i]*=g; nr[nr.size()-1-i]*=g; }
    gBuffer.left = std::move(nl); gBuffer.right = std::move(nr); gPlayheadFrame.store(0); gLoopStartFrame.store(0); gLoopEndFrame.store(gBuffer.frameCount()); return JNI_TRUE;
}
JNIEXPORT jboolean JNICALL Java_id_naga_audioforge_MainActivity_nativeRippleDeleteRange(JNIEnv*, jobject, jdouble inMs, jdouble outMs) {
    std::lock_guard<std::mutex> lk(gMutex); if (gBuffer.left.empty() || outMs <= inMs + 5.0) return JNI_FALSE; gIsPlaying.store(false);
    size_t s = findNearestZeroCrossing(gBuffer.left, (inMs/1000.0)*48000, 48000);
    size_t e = std::min(gBuffer.frameCount(), findNearestZeroCrossing(gBuffer.left, (outMs/1000.0)*48000, 48000));
    if (e <= s || (gBuffer.frameCount()-(e-s)) < 480) return JNI_FALSE;
    gBuffer.left.erase(gBuffer.left.begin()+s, gBuffer.left.begin()+e); gBuffer.right.erase(gBuffer.right.begin()+s, gBuffer.right.begin()+e);
    gPlayheadFrame.store(std::min(s, gBuffer.frameCount()-1)); gLoopStartFrame.store(0); gLoopEndFrame.store(gBuffer.frameCount()); return JNI_TRUE;
}
JNIEXPORT jint JNICALL Java_id_naga_audioforge_MainActivity_nativeStripSilence(JNIEnv*, jobject, jfloat thDb) {
    std::lock_guard<std::mutex> lk(gMutex); if (gBuffer.left.empty()) return 0; gIsPlaying.store(false);
    float th = std::pow(10.0f, thDb / 20.0f); size_t win = 2400, sil = 0; int cnt = 0; std::vector<float> ol, orr;
    for (size_t p = 0; p < gBuffer.frameCount(); p += win) {
        size_t e = std::min(gBuffer.frameCount(), p + win); float rms = 0.0f;
        for (size_t i = p; i < e; ++i) { float m = 0.5f*(gBuffer.left[i]+gBuffer.right[i]); rms += m*m; }
        rms = std::sqrt(rms / std::max<size_t>(1, e-p));
        if (rms < th) { sil++; if (sil <= 3) { ol.insert(ol.end(), gBuffer.left.begin()+p, gBuffer.left.begin()+e); orr.insert(orr.end(), gBuffer.right.begin()+p, gBuffer.right.begin()+e); } else if (sil == 4) cnt++; }
        else { sil = 0; ol.insert(ol.end(), gBuffer.left.begin()+p, gBuffer.left.begin()+e); orr.insert(orr.end(), gBuffer.right.begin()+p, gBuffer.right.begin()+e); }
    }
    if (!ol.empty()) { gBuffer.left = std::move(ol); gBuffer.right = std::move(orr); }
    gPlayheadFrame.store(0); gLoopStartFrame.store(0); gLoopEndFrame.store(gBuffer.frameCount()); return cnt;
}
JNIEXPORT jfloat JNICALL Java_id_naga_audioforge_MainActivity_nativeNormalizeLufs(JNIEnv*, jobject, jfloat target) {
    std::lock_guard<std::mutex> lk(gMutex); if (gBuffer.left.empty()) return 0.0f;
    double sq = 0.0; for (size_t i = 0; i < gBuffer.frameCount(); ++i) sq += 0.5*(gBuffer.left[i]*gBuffer.left[i] + gBuffer.right[i]*gBuffer.right[i]);
    float cur = -0.691f + 10.0f * std::log10(std::max(1e-12, sq / gBuffer.frameCount()));
    float gdb = std::clamp(target - cur, -24.0f, 24.0f), gl = std::pow(10.0f, gdb/20.0f), ceil = std::pow(10.0f, -1.0f/20.0f);
    for (size_t i = 0; i < gBuffer.frameCount(); ++i) { gBuffer.left[i] = std::clamp(gBuffer.left[i]*gl, -ceil, ceil); gBuffer.right[i] = std::clamp(gBuffer.right[i]*gl, -ceil, ceil); }
    return gdb;
}
JNIEXPORT void JNICALL Java_id_naga_audioforge_MainActivity_nativeDenoiseSpectral(JNIEnv*, jobject, jfloat str) {
    std::lock_guard<std::mutex> lk(gMutex); if (gBuffer.left.size() < 2048) return; gIsPlaying.store(false);
    const size_t N = 1024, hop = 512; std::vector<float> win(N); for (size_t i = 0; i < N; ++i) win[i] = std::sin(PI_F * i / N);
    auto proc = [&](std::vector<float>& ch) {
        std::vector<float> np(N, 1e-4f); float minE = 1e30f;
        for (size_t p = 0; p + N < ch.size(); p += hop*4) {
            float e = 0.0f; for (size_t i = 0; i < N; ++i) e += ch[p+i]*ch[p+i];
            if (e < minE && e > 1e-9f) { minE = e; std::vector<std::complex<float>> sp(N); for (size_t i = 0; i < N; ++i) sp[i] = ch[p+i]*win[i]; fftInPlace(sp); for (size_t k = 0; k < N; ++k) np[k] = std::abs(sp[k]); }
        }
        std::vector<float> out(ch.size(), 0.0f); std::vector<std::complex<float>> fr(N);
        for (size_t p = 0; p + N <= ch.size(); p += hop) {
            for (size_t i = 0; i < N; ++i) fr[i] = ch[p+i]*win[i]; fftInPlace(fr);
            for (size_t k = 0; k < N; ++k) { float m = std::abs(fr[k]), nz = np[k]*(1.2f + str*2.5f); fr[k] *= (m > nz) ? std::clamp((m - 0.85f*nz)/m, 0.05f, 1.0f) : 0.08f; }
            for (size_t k = 0; k < N; ++k) fr[k] = std::conj(fr[k]); fftInPlace(fr);
            for (size_t i = 0; i < N; ++i) out[p+i] += (fr[i].real()/N)*win[i];
        }
        ch = std::move(out);
    };
    proc(gBuffer.left); proc(gBuffer.right);
}
JNIEXPORT void JNICALL Java_id_naga_audioforge_MainActivity_nativeApplyMasteringFx(JNIEnv*, jobject, jint eq, jint fi, jint fo) {
    std::lock_guard<std::mutex> lk(gMutex); if (gBuffer.left.empty()) return;
    if (eq > 0) {
        float lpL = 0, lpR = 0, hpL = 0, hpR = 0;
        for (size_t i = 0; i < gBuffer.frameCount(); ++i) {
            float l = gBuffer.left[i], r = gBuffer.right[i];
            lpL += 0.03f*(l - lpL); lpR += 0.03f*(r - lpR); hpL += 0.45f*(l - hpL); hpR += 0.45f*(r - hpR);
            if (eq == 1) { l = (l - 0.4f*lpL) + 0.35f*(l - hpL); r = (r - 0.4f*lpR) + 0.35f*(r - hpR); }
            else if (eq == 2) { l = l + 0.30f*lpL - 0.15f*(l - hpL); r = r + 0.30f*lpR - 0.15f*(r - hpR); }
            gBuffer.left[i] = std::clamp(l, -0.98f, 0.98f); gBuffer.right[i] = std::clamp(r, -0.98f, 0.98f);
        }
    }
    size_t fIn = std::min(gBuffer.frameCount()/2, (size_t)((fi/1000.0)*48000));
    for (size_t i = 0; i < fIn; ++i) { float c = ((float)i/fIn)*((float)i/fIn); gBuffer.left[i]*=c; gBuffer.right[i]*=c; }
    size_t fOut = std::min(gBuffer.frameCount()/2, (size_t)((fo/1000.0)*48000));
    for (size_t i = 0; i < fOut; ++i) { float c = ((float)i/fOut)*((float)i/fOut); size_t x = gBuffer.frameCount()-1-i; gBuffer.left[x]*=c; gBuffer.right[x]*=c; }
}
JNIEXPORT void JNICALL Java_id_naga_audioforge_MainActivity_nativeMidSideProcess(JNIEnv*, jobject, jint mode) {
    std::lock_guard<std::mutex> lk(gMutex); if (gBuffer.left.empty()) return;
    for (size_t i = 0; i < gBuffer.frameCount(); ++i) {
        float m = 0.5f*(gBuffer.left[i]+gBuffer.right[i]), s = 0.5f*(gBuffer.left[i]-gBuffer.right[i]);
        if (mode == 1) { gBuffer.left[i] = std::clamp(s*1.4f, -0.98f, 0.98f); gBuffer.right[i] = std::clamp(-s*1.4f, -0.98f, 0.98f); }
        else if (mode == 2) { gBuffer.left[i] = std::clamp(m, -0.98f, 0.98f); gBuffer.right[i] = std::clamp(m, -0.98f, 0.98f); }
    }
}
JNIEXPORT void JNICALL Java_id_naga_audioforge_MainActivity_nativeApply8DSpatial(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> lk(gMutex); if (gBuffer.left.empty()) return;
    for (size_t i = 0; i < gBuffer.frameCount(); ++i) {
        float pan = 0.5f*(1.0f + std::sin(2.0f*PI_F*0.18f*(i/48000.0f)));
        float m = 0.5f*(gBuffer.left[i]+gBuffer.right[i]);
        gBuffer.left[i] = m * std::cos(pan*0.5f*PI_F); gBuffer.right[i] = m * std::sin(pan*0.5f*PI_F);
    }
}
JNIEXPORT void JNICALL Java_id_naga_audioforge_MainActivity_nativeTimePitchProcess(JNIEnv*, jobject, jfloat tempo, jfloat semi) {
    std::lock_guard<std::mutex> lk(gMutex); if (gBuffer.left.size() < 4096) return; gIsPlaying.store(false);
    float pf = std::pow(2.0f, semi/12.0f), st = std::clamp(tempo/pf, 0.4f, 2.5f);
    auto wsola = [&](const std::vector<float>& in) {
        const size_t win = 1024, syn = 512; size_t ana = std::max<size_t>(64, syn * st);
        std::vector<float> out((size_t)(in.size()/st) + win, 0.0f), h(win);
        for (size_t i = 0; i < win; ++i) h[i] = 0.5f*(1.0f - std::cos(2.0f*PI_F*i/win));
        size_t ip = 0, op = 0;
        while (ip + win < in.size() && op + win < out.size()) { for (size_t i = 0; i < win; ++i) out[op+i] += in[ip+i]*h[i]; ip += ana; op += syn; }
        out.resize(op); return out;
    };
    auto sl = wsola(gBuffer.left), sr = wsola(gBuffer.right);
    if (std::fabs(pf - 1.0f) > 0.005f) { int fr = 48000.0f * pf; resampleChannel(sl, fr, gBuffer.left, 48000); resampleChannel(sr, fr, gBuffer.right, 48000); }
    else { gBuffer.left = std::move(sl); gBuffer.right = std::move(sr); }
    gPlayheadFrame.store(0); gLoopStartFrame.store(0); gLoopEndFrame.store(gBuffer.frameCount());
}
JNIEXPORT jboolean JNICALL Java_id_naga_audioforge_MainActivity_nativeExportAudio(JNIEnv* env, jobject, jstring jp, jint fmt, jint sr, jint br, jint ch) {
    std::lock_guard<std::mutex> lk(gMutex); if (gBuffer.left.empty()) return JNI_FALSE;
    const char* cp = env->GetStringUTFChars(jp, nullptr); std::string p(cp); env->ReleaseStringUTFChars(jp, cp);
    return writeAudioFile(p, gBuffer, sr, fmt, br*1000, ch) ? JNI_TRUE : JNI_FALSE;
}
}
