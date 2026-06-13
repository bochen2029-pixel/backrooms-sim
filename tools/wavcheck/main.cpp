// wavcheck — WAV spectral gate tool (M6).
//
//   wavcheck spectrum <in.wav>
//       Print sample rate, RMS, and band powers (60/120/180 Hz + a noise floor).
//   wavcheck assert <in.wav> [--min-rms R] [--fund-ratio F] [--harm-ratio H]
//       Exit 0 iff RMS >= R (not silent) AND the 60 Hz fundamental and its first
//       two harmonics each exceed the upper-band noise floor by the given ratios.
//
// The FFT is a self-contained iterative radix-2 (no deps); WAV I/O is the shared
// header-only reader. Exit codes: 0 pass, 2 usage, 3 I/O error, 4 assertion fail.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "audio/wav.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

// In-place iterative radix-2 FFT (forward). re/im length must be a power of two.
void fft(std::vector<double>& re, std::vector<double>& im) {
    const size_t n = re.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const double wr = std::cos(ang), wi = std::sin(ang);
        for (size_t i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (size_t k = 0; k < len / 2; ++k) {
                const double ur = re[i + k], ui = im[i + k];
                const double vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                const double vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr; im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
                const double ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr; cr = ncr;
            }
        }
    }
}

struct Analysis {
    uint32_t sample_rate = 0;
    double rms = 0.0;
    double p_fund = 0.0;    // band power around 60 Hz
    double p_h2 = 0.0;      // 120 Hz
    double p_h3 = 0.0;      // 180 Hz
    double p_floor = 0.0;   // mean band power in [1500, 3000] Hz (noise floor)
    bool ok = false;
    std::string err;
};

// Sum |X|^2 over the FFT bins covering [flo, fhi] Hz.
double band_power(const std::vector<double>& re, const std::vector<double>& im,
                  uint32_t sr, double flo, double fhi) {
    const size_t n = re.size();
    const double bin_hz = static_cast<double>(sr) / static_cast<double>(n);
    size_t k0 = static_cast<size_t>(flo / bin_hz);
    size_t k1 = static_cast<size_t>(fhi / bin_hz) + 1;
    if (k1 > n / 2) k1 = n / 2;
    double p = 0.0;
    for (size_t k = k0; k < k1; ++k) p += re[k] * re[k] + im[k] * im[k];
    return p;
}

Analysis analyze(const std::string& path) {
    Analysis a;
    std::vector<int16_t> samples; uint32_t sr = 0; uint16_t ch = 0;
    std::string err;
    if (!br::audio::read_wav(path, samples, sr, ch, err)) { a.err = err; return a; }
    if (ch == 0 || samples.empty()) { a.err = "empty WAV"; return a; }
    a.sample_rate = sr;

    // Downmix to mono float.
    const size_t frames = samples.size() / ch;
    std::vector<double> mono(frames);
    double sumsq = 0.0;
    for (size_t i = 0; i < frames; ++i) {
        double acc = 0.0;
        for (uint16_t c = 0; c < ch; ++c) acc += static_cast<double>(samples[i * ch + c]) / 32768.0;
        acc /= static_cast<double>(ch);
        mono[i] = acc;
        sumsq += acc * acc;
    }
    a.rms = std::sqrt(sumsq / static_cast<double>(frames));

    // Largest power-of-two window up to 2^18, Hann-windowed.
    size_t n = 1;
    while (n * 2 <= frames && n < (1u << 18)) n <<= 1;
    std::vector<double> re(n), im(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n - 1));
        re[i] = mono[i] * w;
    }
    fft(re, im);

    a.p_fund = band_power(re, im, sr, 55.0, 65.0);
    a.p_h2 = band_power(re, im, sr, 115.0, 125.0);
    a.p_h3 = band_power(re, im, sr, 175.0, 185.0);
    // Noise floor: average a 10 Hz-wide band's worth of power across [1500,3000].
    const double floor_total = band_power(re, im, sr, 1500.0, 3000.0);
    a.p_floor = floor_total * (10.0 / 1500.0);  // scale to a ~10 Hz comparison band
    if (a.p_floor <= 0.0) a.p_floor = 1e-12;
    a.ok = true;
    return a;
}

int usage() {
    std::fprintf(stderr,
        "usage:\n"
        "  wavcheck spectrum <in.wav>\n"
        "  wavcheck assert   <in.wav> [--min-rms R] [--fund-ratio F] [--harm-ratio H]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string cmd = argv[1];
    const std::string path = argv[2];

    const Analysis a = analyze(path);
    if (!a.ok) { std::fprintf(stderr, "wavcheck: %s\n", a.err.c_str()); return 3; }

    if (cmd == "spectrum") {
        std::printf("sample_rate: %u\n", a.sample_rate);
        std::printf("rms: %.6f\n", a.rms);
        std::printf("p_fund_60: %.6g\n", a.p_fund);
        std::printf("p_harm_120: %.6g\n", a.p_h2);
        std::printf("p_harm_180: %.6g\n", a.p_h3);
        std::printf("p_floor: %.6g\n", a.p_floor);
        std::printf("fund_over_floor: %.2f\n", a.p_fund / a.p_floor);
        return 0;
    }
    if (cmd == "assert") {
        double min_rms = 0.01, fund_ratio = 8.0, harm_ratio = 3.0;
        for (int i = 3; i + 1 < argc; i += 2) {
            if (std::strcmp(argv[i], "--min-rms") == 0) min_rms = std::atof(argv[i + 1]);
            else if (std::strcmp(argv[i], "--fund-ratio") == 0) fund_ratio = std::atof(argv[i + 1]);
            else if (std::strcmp(argv[i], "--harm-ratio") == 0) harm_ratio = std::atof(argv[i + 1]);
        }
        int rc = 0;
        if (a.rms < min_rms) {
            std::fprintf(stderr, "FAIL: rms %.5f < %.5f (silent)\n", a.rms, min_rms); rc = 4;
        }
        if (a.p_fund < a.p_floor * fund_ratio) {
            std::fprintf(stderr, "FAIL: 60 Hz fundamental %.4g < floor*%.1f (%.4g)\n",
                         a.p_fund, fund_ratio, a.p_floor * fund_ratio); rc = 4;
        }
        if (a.p_h2 < a.p_floor * harm_ratio) {
            std::fprintf(stderr, "FAIL: 120 Hz harmonic %.4g < floor*%.1f (%.4g)\n",
                         a.p_h2, harm_ratio, a.p_floor * harm_ratio); rc = 4;
        }
        if (a.p_h3 < a.p_floor * harm_ratio) {
            std::fprintf(stderr, "FAIL: 180 Hz harmonic %.4g < floor*%.1f (%.4g)\n",
                         a.p_h3, harm_ratio, a.p_floor * harm_ratio); rc = 4;
        }
        if (rc == 0) {
            std::printf("OK rms=%.5f fund/floor=%.1f h2/floor=%.1f h3/floor=%.1f\n",
                        a.rms, a.p_fund / a.p_floor, a.p_h2 / a.p_floor, a.p_h3 / a.p_floor);
        }
        return rc;
    }
    return usage();
}
