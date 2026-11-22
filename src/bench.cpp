// --bench: scores frame generation against ground truth using the shipping GPU code.
//
// Input: a folder of clips, each a subfolder of consecutive 60 fps frames (0000.png, 0001.png, ...)
// from a game that really renders at 60. Even frames are fed in as a 30 fps game would arrive
// (motion hints from the previous pair included, exactly like live play); each generated midpoint
// is compared with the real odd frame between them.
//
//   FrameLift.exe --bench <clips dir> --configs <file> --out <results.csv>
//
// configs file: one per line, "name key=value ...", keys = Tuning fields (see framegen.h).
#include "bench.h"

#include "framegen.h"
#include "log.h"

#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace {

bool LoadBGRA(IWICImagingFactory* wic, const std::wstring& path, int& w, int& h, std::vector<uint8_t>& out) {
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapSource> bgra;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    if (FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, frame.Get(), &bgra))) return false;
    UINT uw = 0, uh = 0;
    bgra->GetSize(&uw, &uh);
    w = (int)uw;
    h = (int)uh;
    out.resize((size_t)w * h * 4);
    return SUCCEEDED(bgra->CopyPixels(nullptr, (UINT)w * 4, (UINT)out.size(), out.data()));
}

std::vector<std::wstring> ListDir(const std::wstring& dir, bool dirs) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (dirs == isDir && (dirs || Lower(n).size() > 4 && Lower(n).substr(n.size() - 4) == L".png")) out.push_back(n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

bool gOracle = false;
bool gScaleCheck = false;
bool gHdr = false;
bool gDrawCheck = false;  // log 4K draw cost: direct vs source-res + scale-up  // also report the best-possible per-8x8-block choice of {blend, A only, B only}

// HDR test input: encode an SDR BGRA frame as HDR10 P010 (sRGB -> linear -> BT.2020 ->
// 203-nit graphics white -> PQ -> BT.2020 YCbCr 10-bit limited, 4:2:0), the inverse of what the
// GPU decodes. A perfect decoder returns the original except for the highlight roll-off.
std::vector<uint8_t> ToP010(const std::vector<uint8_t>& bgra, int W, int H) {
    static float srgb[256];
    static std::vector<float> pq;
    if (pq.empty()) {
        for (int i = 0; i < 256; i++) {
            float c = i / 255.0f;
            srgb[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        pq.resize(65537);
        for (int i = 0; i <= 65536; i++) {
            double y = (i / 65536.0) * 203.0 / 10000.0;
            const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
            double p = std::pow(y, m1);
            pq[i] = (float)std::pow((c1 + c2 * p) / (1 + c3 * p), m2);
        }
    }
    auto PQ = [&](float lin) { return pq[(size_t)std::clamp((int)std::lround(lin * 65536.0f), 0, 65536)]; };
    std::vector<uint8_t> out((size_t)W * H * 2 + (size_t)W * ((H + 1) / 2) * 2);
    uint16_t* Y = (uint16_t*)out.data();
    uint16_t* UV = Y + (size_t)W * H;
    std::vector<float> cb((size_t)W * H), cr((size_t)W * H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint8_t* p = &bgra[((size_t)y * W + x) * 4];
            float r = srgb[p[2]], g = srgb[p[1]], b = srgb[p[0]];
            float R = PQ(0.6274f * r + 0.3293f * g + 0.0433f * b), G = PQ(0.0691f * r + 0.9195f * g + 0.0114f * b),
                  B = PQ(0.0164f * r + 0.0880f * g + 0.8956f * b);
            float yy = 0.2627f * R + 0.6780f * G + 0.0593f * B;
            Y[(size_t)y * W + x] = (uint16_t)(std::clamp((int)std::lround(64 + 876 * yy), 0, 1023) << 6);
            cb[(size_t)y * W + x] = (B - yy) / 1.8814f;
            cr[(size_t)y * W + x] = (R - yy) / 1.4746f;
        }
    for (int y = 0; y < H; y += 2)
        for (int x = 0; x < W; x += 2) {
            float u = 0, v = 0;
            int n = 0;
            for (int dy = 0; dy < 2 && y + dy < H; dy++)
                for (int dx = 0; dx < 2 && x + dx < W; dx++, n++) {
                    u += cb[(size_t)(y + dy) * W + x + dx];
                    v += cr[(size_t)(y + dy) * W + x + dx];
                }
            uint16_t* o = UV + (size_t)(y / 2) * W + x;
            o[0] = (uint16_t)(std::clamp((int)std::lround(512 + 896 * u / n), 0, 1023) << 6);
            o[1] = (uint16_t)(std::clamp((int)std::lround(512 + 896 * v / n), 0, 1023) << 6);
        }
    return out;
}

bool ParseConfig(const std::string& line, std::string& name, Tuning& t) {
    std::istringstream ss(line);
    if (!(ss >> name) || name[0] == '#') return false;
    std::string kv;
    while (ss >> kv) {
        auto eq = kv.find('=');
        if (eq == std::string::npos) continue;
        std::string k = kv.substr(0, eq);
        double v = atof(kv.substr(eq + 1).c_str());
        if (k == "lambda") t.lambda = (float)v;
        else if (k == "radius16") t.radius16 = (int)v;
        else if (k == "local") t.local = (int)v;
        else if (k == "localpen") t.localPen = (float)v;
        else if (k == "bias") t.refineBias = (float)v;
        else if (k == "zerobias") t.zeroBias = (float)v;
        else if (k == "median") t.denseMedian = v != 0;
        else if (k == "occl") t.occlusion = v != 0;
        else if (k == "occtol") t.occTol = (float)v;
        else if (k == "occspan") t.occSpan = (float)v;
        else if (k == "blendonly") t.blendOnly = v != 0;
        else if (k == "oracle") gOracle = v != 0;
        else if (k == "drawcheck") gDrawCheck = v != 0;
        else if (k == "scalecheck") gScaleCheck = v != 0;
        else if (k == "hdr") gHdr = v != 0;
        else if (k == "motionscale") t.motionScale = (int)v;
        else if (k == "hp") t.matchHP = (int)v;
        else if (k == "engine") t.engine = (int)v;
        else if (k == "nvperf") t.nvPerf = (int)v;
        else if (k == "hybrid") t.nvHybrid = v != 0;
        else if (k == "nvbias") t.nvBias = (float)v;
        else if (k == "nvbilin") t.nvBilinear = v != 0;
        else if (k == "nvscale") t.nvScale = (int)v;
        else if (k == "static") t.staticThr = (float)(v / 255.0);
    }
    return true;
}

double Psnr(double sse, double n) { return n > 0 ? 10.0 * std::log10(1.0 / std::max(sse / n, 1e-12)) : 0; }

// Mean SSIM on luma with an 8x8 box window (summed-area tables), C1/C2 as in Wang et al.
double Ssim(const std::vector<float>& x, const std::vector<float>& y, int w, int h) {
    const int W1 = w + 1;
    std::vector<double> sx((size_t)W1 * (h + 1)), sy(sx.size()), sxx(sx.size()), syy(sx.size()), sxy(sx.size());
    for (int j = 0; j < h; j++) {
        double rx = 0, ry = 0, rxx = 0, ryy = 0, rxy = 0;
        for (int i = 0; i < w; i++) {
            double a = x[(size_t)j * w + i], b = y[(size_t)j * w + i];
            rx += a; ry += b; rxx += a * a; ryy += b * b; rxy += a * b;
            size_t o = (size_t)(j + 1) * W1 + (i + 1), u = (size_t)j * W1 + (i + 1);
            sx[o] = sx[u] + rx; sy[o] = sy[u] + ry; sxx[o] = sxx[u] + rxx; syy[o] = syy[u] + ryy; sxy[o] = sxy[u] + rxy;
        }
    }
    auto box = [&](const std::vector<double>& s, int x0, int y0, int x1, int y1) {
        return s[(size_t)y1 * W1 + x1] - s[(size_t)y0 * W1 + x1] - s[(size_t)y1 * W1 + x0] + s[(size_t)y0 * W1 + x0];
    };
    const double C1 = 0.01 * 0.01, C2 = 0.03 * 0.03, n = 64;
    double total = 0;
    int count = 0;
    for (int j = 0; j + 8 <= h; j += 4)
        for (int i = 0; i + 8 <= w; i += 4) {
            double mx = box(sx, i, j, i + 8, j + 8) / n, my = box(sy, i, j, i + 8, j + 8) / n;
            double vx = box(sxx, i, j, i + 8, j + 8) / n - mx * mx, vy = box(syy, i, j, i + 8, j + 8) / n - my * my;
            double cxy = box(sxy, i, j, i + 8, j + 8) / n - mx * my;
            total += ((2 * mx * my + C1) * (2 * cxy + C2)) / ((mx * mx + my * my + C1) * (vx + vy + C2));
            count++;
        }
    return count ? total / count : 0;
}

}  // namespace

bool LoadPngBGRA(const std::wstring& path, int& w, int& h, std::vector<uint8_t>& out) {
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    return LoadBGRA(wic.Get(), path, w, h, out);
}

int RunBench(const std::wstring& clipsDir, const std::wstring& configsPath, const std::wstring& outPath, int stride) {
    LogFile() = _wfopen((outPath + L".log").c_str(), L"w");  // console output doesn't survive SSH
    stride = std::max(2, stride - stride % 2);  // even: the ground truth is the frame halfway between
    Renderer r;
    if (!r.InitHeadless()) {
        fprintf(stderr, "bench: no D3D11 device\n");
        return 2;
    }
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
        fprintf(stderr, "bench: no WIC\n");
        return 2;
    }
    std::vector<std::pair<std::string, Tuning>> configs;
    std::vector<bool> oracleFlags, drawFlags, scaleFlags, hdrFlags;
    if (FILE* cf = configsPath.empty() ? nullptr : _wfopen(configsPath.c_str(), L"r")) {
        char buf[1024];
        while (fgets(buf, sizeof(buf), cf)) {
            std::string name;
            Tuning t;
            gOracle = false;
            gDrawCheck = false;
            gScaleCheck = false;
            gHdr = false;
            if (ParseConfig(buf, name, t)) {
                configs.push_back({name, t});
                oracleFlags.push_back(gOracle);
                drawFlags.push_back(gDrawCheck);
                scaleFlags.push_back(gScaleCheck);
                hdrFlags.push_back(gHdr);
            }
        }
        fclose(cf);
    }
    if (configs.empty()) {
        configs.push_back({"default", Tuning{}});
        oracleFlags.push_back(false);
        drawFlags.push_back(false);
        scaleFlags.push_back(false);
        hdrFlags.push_back(false);
    }
    FILE* out = _wfopen(outPath.c_str(), L"w");
    if (!out) return 2;
    fprintf(out, "clip,config,psnr_all,psnr_moving,ssim,worst5,pairs\n");

    struct Acc { std::vector<double> clipAll, clipMov, clipSsim, clipWorst; };
    std::vector<Acc> acc(configs.size());
    const double t0 = NowMs();
    for (const auto& clip : ListDir(clipsDir, true)) {
        const std::wstring cdir = clipsDir + L"\\" + clip;
        auto files = ListDir(cdir, false);
        std::vector<std::vector<uint8_t>> fr;
        int w = 0, h = 0;
        for (auto& fn : files) {
            std::vector<uint8_t> px;
            int fw, fh;
            if (!LoadBGRA(wic.Get(), cdir + L"\\" + fn, fw, fh, px)) break;
            if (fr.empty()) { w = fw; h = fh; }
            if (fw != w || fh != h) break;
            fr.push_back(std::move(px));
        }
        if (fr.size() < 7) continue;
        // Pixels that change between the two real frames: where interpolation actually matters.
        for (size_t ci = 0; ci < configs.size(); ci++) {
            r.tuning = configs[ci].second;
            gOracle = oracleFlags[ci];
            gDrawCheck = drawFlags[ci];
            gScaleCheck = scaleFlags[ci];
            gHdr = hdrFlags[ci];
            r.ResetTimeline();
            double sseAll = 0, nAll = 0, sseMov = 0, nMov = 0, ssimSum = 0, pairMs = 0;
            int pairs = 0;
            std::vector<uint8_t> mid;
            std::vector<float> lo((size_t)w * h), lg((size_t)w * h);
            std::vector<double> tileMse;  // every 64x64 tile of every scored frame
            const int TS = 64, tw = w / TS, th = h / TS;
            for (size_t k = 0; k < fr.size(); k += (size_t)stride) {
                Frame f;
                f.data = gHdr ? ToP010(fr[k], w, h) : fr[k];
                f.width = w;
                f.height = h;
                f.fmt = gHdr ? PixFmt::P010 : PixFmt::BGRA;
                f.transfer = gHdr ? 1 : 0;
                f.bt2020 = gHdr;
                f.ts = k * (1000.0 / 60.0) * (2.0 / stride);  // keep the nominal 33 ms spacing between "real" frames
                const double tp0 = NowMs();
                r.Upload(f, true, 1e9);
                if (k < (size_t)stride * 2) continue;  // warm-up: the first pair has no motion hint yet (live play always does)
                if (!r.RenderNewestMid(mid, 0.5f)) continue;
                pairMs += NowMs() - tp0;
                if (gDrawCheck && k == (size_t)stride * 4) {
                    double md = 0, ms = 0, ps = 0;
                    if (r.DrawCheck(md, ms, ps))
                        Log("drawcheck %s: 4K direct %.2f ms, source-res + scale-up %.2f ms, images match at %.1f dB", Narrow(clip).c_str(), md, ms, ps);
                }
                if (gScaleCheck && k == (size_t)stride * 4) {
                    double o[6] = {};
                    if (r.ScaleCheck(o))
                        Log("scalecheck %s: 2/3 shrink then enlarge, PSNR bilinear %.2f, sharp(0) %.2f, sharp(50) %.2f dB | "
                            "1080p->4K bilinear %.2f ms, sharp %.2f ms | ->1440p sharp %.2f ms",
                            Narrow(clip).c_str(), o[0], o[1], o[2], o[3], o[4], o[5]);
                }
                if (gOracle) {
                    // Replace each 8x8 block of the output with whichever candidate is closest to the truth.
                    std::vector<uint8_t> ca, cb;
                    r.RenderNewestMid(ca, 0.5f, 5);
                    r.RenderNewestMid(cb, 0.5f, 6);
                    const uint8_t* gtp = fr[k - stride / 2].data();
                    for (int by = 0; by + 8 <= h; by += 8)
                        for (int bx = 0; bx + 8 <= w; bx += 8) {
                            double e[3] = {0, 0, 0};
                            const std::vector<uint8_t>* cand[3] = {&mid, &ca, &cb};
                            for (int y = by; y < by + 8; y++)
                                for (int x = bx; x < bx + 8; x++) {
                                    size_t i = (size_t)y * w + x;
                                    for (int c = 0; c < 3; c++)
                                        for (int q = 0; q < 3; q++) {
                                            double d = (*cand[c])[i * 4 + q] - gtp[i * 4 + 2 - q];
                                            e[c] += d * d;
                                        }
                                }
                            int best = e[1] < e[0] ? (e[2] < e[1] ? 2 : 1) : (e[2] < e[0] ? 2 : 0);
                            if (best)
                                for (int y = by; y < by + 8; y++) memcpy(&mid[((size_t)y * w + bx) * 4], &(*cand[best])[((size_t)y * w + bx) * 4], 32);
                        }
                }
                const uint8_t* gt = fr[k - stride / 2].data();
                const uint8_t* A = fr[k - stride].data();
                const uint8_t* B = fr[k].data();
                std::vector<double> tiles((size_t)tw * th, 0.0);
                for (size_t i = 0, n = (size_t)w * h; i < n; i++) {
                    const uint8_t* g = gt + i * 4;  // BGRA
                    const uint8_t* m = mid.data() + i * 4;  // RGBA
                    double e = 0;
                    for (int c = 0; c < 3; c++) {
                        double d = (m[c] - g[2 - c]) / 255.0;
                        e += d * d;
                    }
                    sseAll += e;
                    nAll += 3;
                    lo[i] = (0.299f * m[0] + 0.587f * m[1] + 0.114f * m[2]) / 255.0f;
                    lg[i] = (0.299f * g[2] + 0.587f * g[1] + 0.114f * g[0]) / 255.0f;
                    int tx = (int)(i % w) / TS, ty = (int)(i / w) / TS;
                    if (tx < tw && ty < th) tiles[(size_t)ty * tw + tx] += e;
                    int la = (A[i * 4 + 2] * 77 + A[i * 4 + 1] * 150 + A[i * 4] * 29) >> 8;
                    int lb = (B[i * 4 + 2] * 77 + B[i * 4 + 1] * 150 + B[i * 4] * 29) >> 8;
                    if (std::abs(la - lb) > 5) {
                        sseMov += e;
                        nMov += 3;
                    }
                }
                for (double t : tiles) tileMse.push_back(t / (TS * TS * 3));
                ssimSum += Ssim(lo, lg, w, h);
                pairs++;
            }
            double pa = Psnr(sseAll, nAll), pm = Psnr(sseMov, nMov), ss = pairs ? ssimSum / pairs : 0;
            // Worst 5% of tiles: where local artifacts (a torn head, fuzz beside a character) live.
            std::sort(tileMse.begin(), tileMse.end(), std::greater<double>());
            size_t nw = std::max<size_t>(1, tileMse.size() / 20);
            double worstMse = 0;
            for (size_t i = 0; i < nw && i < tileMse.size(); i++) worstMse += tileMse[i];
            double pw = Psnr(worstMse, (double)nw);
            acc[ci].clipAll.push_back(pa);
            acc[ci].clipMov.push_back(pm);
            acc[ci].clipSsim.push_back(ss);
            acc[ci].clipWorst.push_back(pw);
            fprintf(out, "%s,%s,%.3f,%.3f,%.5f,%.3f,%d\n", Narrow(clip).c_str(), configs[ci].first.c_str(), pa, pm, ss, pw, pairs);
            Log("%s %s: %.2f ms per pair (upload + motion + midpoint + readback), engine %s", Narrow(clip).c_str(),
                configs[ci].first.c_str(), pairs ? pairMs / pairs : 0.0, r.EngineName());
            fflush(out);
        }
        printf("%s: %zu frames, %.0f s elapsed\n", Narrow(clip).c_str(), fr.size(), (NowMs() - t0) / 1000.0);
        fflush(stdout);
    }
    printf("\n%-34s %9s %9s %9s %9s\n", "config", "all", "moving", "ssim", "worst5");
    for (size_t ci = 0; ci < configs.size(); ci++) {
        auto mean = [](const std::vector<double>& v) {
            double s = 0;
            for (double x : v) s += x;
            return v.empty() ? 0.0 : s / v.size();
        };
        printf("%-34s %9.3f %9.3f %9.5f %9.3f\n", configs[ci].first.c_str(), mean(acc[ci].clipAll), mean(acc[ci].clipMov),
               mean(acc[ci].clipSsim), mean(acc[ci].clipWorst));
        fprintf(out, "MEAN,%s,%.3f,%.3f,%.5f,%.3f,%zu\n", configs[ci].first.c_str(), mean(acc[ci].clipAll), mean(acc[ci].clipMov),
                mean(acc[ci].clipSsim), mean(acc[ci].clipWorst), acc[ci].clipAll.size());
    }
    fclose(out);
    return 0;
}
