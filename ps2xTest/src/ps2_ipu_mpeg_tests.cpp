#include "MiniTest.h"
#include "runtime/ps2_ipu.h"
#include "ipu_mpeg_fixtures.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

// IPU decoding of real MPEG-1/MPEG-2 intra slices (tools/ipu/make_ipu_fixtures.py: ffmpeg encodes, and its
// floating-point IDCT decodes the reference). Agreement checks the standard's semantics only: the reference is
// another decoder, not a PS2.
namespace
{
    using Qword = PS2Ipu::Qword;
    using Raw = std::array<int16_t, 384>;
    using ipu_mpeg_fixtures::Fixture;
    using ipu_mpeg_fixtures::Slice;

    constexpr uint32_t kBclr = 0u << 28, kBdec = 2u << 28, kVdec = 3u << 28, kFdec = 4u << 28, kSetiq = 5u << 28;
    constexpr uint32_t kMbPerSlice = ipu_mpeg_fixtures::kWidth / 16u;
    constexpr size_t kRawBytes = 48u * 16u;
    constexpr size_t kNoPause = static_cast<size_t>(-1);

    // Feeds a byte stream to the input FIFO as toIPU DMA would: whole qwords, only while the FIFO has room. With
    // pauseAt set, feeding stops before that qword until the driver resumes it.
    struct Feeder
    {
        std::vector<uint8_t> bytes;
        size_t next = 0u;
        size_t pauseAt = kNoPause;

        void load(const uint8_t *data, size_t size)
        {
            bytes.assign(data, data + size);
            next = 0u;
        }
        [[nodiscard]] size_t qwords() const { return (bytes.size() + 15u) / 16u; }
        [[nodiscard]] bool paused() const { return next == pauseAt && next < qwords(); }
        bool feed(PS2Ipu &ipu)
        {
            if (next >= qwords() || paused() || !ipu.canAcceptInput())
                return false;
            Qword q{};
            const size_t offset = next * 16u;
            std::memcpy(q.data(), bytes.data() + offset, std::min<size_t>(16u, bytes.size() - offset));
            ipu.pushInput(q);
            ++next;
            return true;
        }
    };

    // Plays the guest: issues commands, feeds input only when the IPU waits for it (lazy, so symbols meet an
    // exhausted FIFO at qword boundaries) or as much as fits (eager), and drains the output FIFO.
    struct Driver
    {
        PS2Ipu ipu;
        Feeder feeder;
        bool eager = false;
        std::vector<uint8_t> out;
        std::function<void(Driver &)> onPause; // called once when the IPU waits at the pause point

        void drain()
        {
            Qword q{};
            while (ipu.popOutput(q))
            {
                out.insert(out.end(), q.begin(), q.end());
                ipu.run();
            }
        }
        // Runs one command; false when it still waits for input after the stream ended.
        bool exec(uint32_t command)
        {
            ipu.writeCommand(command);
            for (;;)
            {
                if (eager)
                    while (feeder.feed(ipu))
                        ipu.run();
                ipu.run();
                drain();
                if (!ipu.busy())
                    return true;
                if (feeder.feed(ipu))
                    continue;
                if (!feeder.paused())
                    return false;
                if (onPause)
                    onPause(*this);
                feeder.pauseAt = kNoPause;
            }
        }
        [[nodiscard]] bool error() const { return (ipu.readControl() & (1u << 14)) != 0u; }
        [[nodiscard]] uint32_t result() const { return static_cast<uint32_t>(ipu.readCommand()); }
        void loadMatrix(const uint8_t *matrix)
        {
            const size_t pause = feeder.pauseAt;
            feeder.pauseAt = kNoPause;
            exec(kBclr);
            feeder.load(matrix, 64u);
            exec(kSetiq);
            feeder.pauseAt = pause;
        }
    };

    enum class Outcome
    {
        Done,
        Error,
        Stalled
    };

    // Reads macroblock_address_increment with VDEC (escape adds 33, stuffing is skipped); 0 at the end of the slice
    // (VDEC finds no code and the next 23 bits are zero).
    Outcome readAddressIncrement(Driver &d, uint32_t skip, uint32_t &increment)
    {
        increment = 0u;
        for (;;)
        {
            if (!d.exec(kVdec | skip))
                return Outcome::Stalled;
            skip = 0u;
            if (d.error())
                return (d.ipu.readTop() & 0xFFFFFE00u) == 0u && increment == 0u ? Outcome::Done : Outcome::Error;
            const uint32_t value = d.result() & 0xFFFFu;
            if (value == 34u)
                continue;
            increment += value == 35u ? 33u : value;
            if (value != 35u)
                return Outcome::Done;
        }
    }

    struct SliceCounts
    {
        size_t fieldDct = 0u;   // macroblocks with dct_type 1
        size_t quantiser = 0u;  // macroblocks carrying quantiser_scale_code
    };

    // Decodes one slice as PS2 software does with VDEC/FDEC/BDEC; one RAW16 macroblock per entry of `columns`.
    Outcome decodeSliceWithBdec(Driver &d, const Fixture &f, const Slice &s, std::vector<uint32_t> &columns,
                                SliceCounts *counts = nullptr)
    {
        d.ipu.writeControl(f.control | (1u << 24)); // PCT = I
        d.loadMatrix(f.intraMatrix);
        d.exec(kBclr);
        d.feeder.load(s.data, s.size);
        uint32_t increment = 0u;
        Outcome o = readAddressIncrement(d, s.headerBits, increment);
        if (o != Outcome::Done || increment == 0u)
            return o == Outcome::Done ? Outcome::Error : o;
        uint32_t column = increment - 1u;
        uint32_t qsc = s.qsc;
        bool first = true;
        for (;;)
        {
            if (!d.exec(kVdec | (1u << 26)))
                return Outcome::Stalled;
            if (d.error())
                return Outcome::Error;
            const uint32_t type = d.result() & 0xFFFFu;
            uint32_t skip = 0u; // VDEC consumed its symbol; FDEC does not, so its bits are skipped by the next command
            uint32_t field = 0u;
            if (f.dtd)
            {
                if (!d.exec(kFdec | skip))
                    return Outcome::Stalled;
                field = d.result() >> 31;
                skip = 1u;
                if (counts != nullptr)
                    counts->fieldDct += field;
            }
            if ((type & 0x10u) != 0u)
            {
                if (!d.exec(kFdec | skip))
                    return Outcome::Stalled;
                qsc = d.result() >> 27;
                skip = 5u;
                if (counts != nullptr)
                    ++counts->quantiser;
            }
            if (!d.exec(kBdec | skip | (qsc << 16) | (field << 25) | (first ? (1u << 26) : 0u) | (1u << 27)))
                return Outcome::Stalled;
            if (d.error())
                return Outcome::Error;
            columns.push_back(column);
            first = false;
            o = readAddressIncrement(d, 0u, increment);
            if (o != Outcome::Done)
                return o;
            if (increment == 0u)
                return Outcome::Done;
            if (increment != 1u)
                return Outcome::Error; // I pictures skip no macroblocks
            column += increment;
        }
    }

    Raw rawAt(const std::vector<uint8_t> &out, size_t mb)
    {
        Raw raw{};
        std::memcpy(raw.data(), out.data() + mb * kRawBytes, sizeof(raw));
        return raw;
    }

    // Reference sample i of macroblock (row, column): Y 16x16, then Cb, Cr 8x8 of the 4:2:0 picture.
    int referenceSample(const Fixture &f, uint32_t row, uint32_t column, uint32_t i)
    {
        const uint32_t w = ipu_mpeg_fixtures::kWidth, h = ipu_mpeg_fixtures::kHeight;
        if (i < 256u)
            return f.yuv[(row * 16u + i / 16u) * w + column * 16u + i % 16u];
        const uint32_t c = (i - 256u) % 64u, plane = (i - 256u) / 64u;
        return f.yuv[w * h + plane * (w / 2u) * (h / 2u) + (row * 8u + c / 8u) * (w / 2u) + column * 8u + c % 8u];
    }

    struct Picture
    {
        Outcome outcome = Outcome::Done;
        std::vector<uint8_t> out; // RAW16 macroblocks in slice order
        std::vector<uint32_t> rows, columns;
        SliceCounts counts;
    };

    Picture decodePictureWithBdec(const Fixture &f, bool eager)
    {
        Picture p;
        for (size_t i = 0; i < f.sliceCount && p.outcome == Outcome::Done; ++i)
        {
            Driver d;
            d.eager = eager;
            std::vector<uint32_t> columns;
            p.outcome = decodeSliceWithBdec(d, f, f.slices[i], columns, &p.counts);
            p.out.insert(p.out.end(), d.out.begin(), d.out.end());
            for (uint32_t c : columns)
            {
                p.rows.push_back(f.slices[i].row);
                p.columns.push_back(c);
            }
        }
        return p;
    }

    // Deterministic xorshift for the malformed-stream cases.
    uint32_t nextRandom(uint32_t &state)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
}

void register_ps2_ipu_mpeg_tests()
{
    MiniTest::Case("PS2IpuMpeg", [](TestCase &tc)
    {
        // IEEE 1180-1990 bounds for an inverse DCT against the exact one: peak error 1, overall mean square error
        // 0.02. The reference decoder's IDCT is itself approximate, so the bounds apply to the pair.
        tc.Run("BDEC driven by VDEC/FDEC decodes ffmpeg-encoded MPEG-1/MPEG-2 intra pictures within IEEE 1180 bounds", [](TestCase &t)
        {
            SliceCounts total;
            for (const Fixture &f : ipu_mpeg_fixtures::kFixtures)
            {
                const Picture p = decodePictureWithBdec(f, false);
                total.fieldDct += p.counts.fieldDct;
                total.quantiser += p.counts.quantiser;
                const std::string name = f.name;
                t.IsTrue(p.outcome == Outcome::Done, name + ": every slice decodes to its end");
                t.Equals(p.columns.size(), static_cast<size_t>(f.sliceCount * kMbPerSlice), name + ": every macroblock");
                t.Equals(p.out.size(), p.columns.size() * kRawBytes, name + ": 48 RAW16 qwords per macroblock");
                int peak = 0;
                double squares = 0.0;
                size_t samples = 0u, exact = 0u;
                for (size_t mb = 0; mb < p.columns.size(); ++mb)
                {
                    const Raw raw = rawAt(p.out, mb);
                    for (uint32_t i = 0; i < 384u; ++i)
                    {
                        const int e = raw[i] - referenceSample(f, p.rows[mb], p.columns[mb], i);
                        peak = std::max(peak, std::abs(e));
                        squares += static_cast<double>(e) * e;
                        exact += e == 0 ? 1u : 0u;
                        ++samples;
                    }
                }
                const double mse = squares / static_cast<double>(std::max<size_t>(samples, 1u));
                std::cout << "    [ipu-fixture] " << name << ": " << p.columns.size() << " macroblocks, field DCT "
                          << p.counts.fieldDct << ", quantiser changes " << p.counts.quantiser << ", peak " << peak
                          << ", mse " << mse << ", exact " << exact << "/" << samples << "\n";
                t.IsTrue(samples != 0u && peak <= 1, name + ": peak error " + std::to_string(peak) + " (at most 1)");
                t.IsTrue(samples != 0u && mse <= 0.02, name + ": mean square error " + std::to_string(mse) + ", " +
                                                           std::to_string(exact) + "/" + std::to_string(samples) + " exact");
            }
            t.IsTrue(total.fieldDct != 0u && total.quantiser != 0u,
                     "the fixtures exercise field DCT and per-macroblock quantiser_scale_code");
        });

        tc.Run("BDEC decoding is repeatable and independent of how the input FIFO is filled", [](TestCase &t)
        {
            for (const Fixture &f : ipu_mpeg_fixtures::kFixtures)
            {
                const Picture lazy = decodePictureWithBdec(f, false);
                const Picture again = decodePictureWithBdec(f, false);
                const Picture eager = decodePictureWithBdec(f, true);
                t.IsTrue(!lazy.out.empty() && lazy.out == again.out, std::string(f.name) + ": the same stream decodes the same way twice");
                t.IsTrue(lazy.out == eager.out, std::string(f.name) + ": qword-by-qword and full-FIFO feeding agree");
            }
        });

        tc.Run("BDEC paused at every qword of a slice waits without error or partial output, then resumes exactly", [](TestCase &t)
        {
            for (const Fixture &f : ipu_mpeg_fixtures::kFixtures)
            {
                const Slice &s = f.slices[0];
                Driver whole;
                std::vector<uint32_t> columns;
                t.IsTrue(decodeSliceWithBdec(whole, f, s, columns) == Outcome::Done, "the whole slice decodes");
                const size_t qwords = (s.size + 15u) / 16u;
                size_t pauses = 0u, clean = 0u;
                for (size_t cut = 1u; cut < qwords; ++cut)
                {
                    Driver d;
                    d.feeder.pauseAt = cut;
                    d.onPause = [&](Driver &paused)
                    {
                        ++pauses;
                        const bool waiting = paused.ipu.busy() && !paused.error();
                        const bool wholeMacroblocks = paused.out.size() % kRawBytes == 0u;
                        const bool fifoAccounted = ((paused.ipu.readBitPointer() >> 8) & 0xFu) + ((paused.ipu.readBitPointer() >> 16) & 3u) ==
                                                   paused.ipu.inputQwords();
                        clean += waiting && wholeMacroblocks && fifoAccounted ? 1u : 0u;
                    };
                    std::vector<uint32_t> resumedColumns;
                    const bool done = decodeSliceWithBdec(d, f, s, resumedColumns) == Outcome::Done;
                    t.IsTrue(done && d.out == whole.out && resumedColumns == columns,
                             std::string(f.name) + ": resumed after qword " + std::to_string(cut) + " decodes identically");
                }
                t.IsTrue(pauses != 0u && clean == pauses, std::string(f.name) + ": every pause found the IPU waiting cleanly (" +
                                                              std::to_string(clean) + "/" + std::to_string(pauses) + ")");
            }
        });

        tc.Run("BDEC on a truncated slice keeps waiting for data: no error, no partial macroblock", [](TestCase &t)
        {
            const Fixture &f = ipu_mpeg_fixtures::kFixtures[2];
            const Slice &s = f.slices[0];
            for (size_t keep = 16u; keep < s.size; keep += 16u)
            {
                Slice cut = s;
                cut.size = keep; // the stream ends inside the slice, without its terminating start code
                Driver d;
                std::vector<uint32_t> columns;
                const Outcome o = decodeSliceWithBdec(d, f, cut, columns);
                t.IsTrue(o == Outcome::Stalled && d.ipu.busy() && !d.error() && d.out.size() == columns.size() * kRawBytes,
                         "truncated after " + std::to_string(keep) + " bytes: the command waits, only whole macroblocks came out");
            }
        });

        tc.Run("CTRL.RST in the middle of a slice leaves no state behind: the slice then decodes as from cold", [](TestCase &t)
        {
            for (const Fixture &f : ipu_mpeg_fixtures::kFixtures)
            {
                const Slice &s = f.slices[0];
                Driver cold;
                std::vector<uint32_t> columns;
                (void)decodeSliceWithBdec(cold, f, s, columns);
                Driver d;
                d.feeder.pauseAt = (s.size + 15u) / 32u; // half way through the slice
                bool reset = false;
                d.onPause = [&](Driver &paused)
                {
                    paused.ipu.writeControl(1u << 30);
                    reset = !paused.ipu.busy() && paused.ipu.inputQwords() == 0u && paused.ipu.outputQwords() == 0u &&
                            paused.ipu.readBitPointer() == 0u;
                };
                std::vector<uint32_t> ignored;
                (void)decodeSliceWithBdec(d, f, s, ignored); // continues on a reset IPU: whatever it decodes is discarded
                t.IsTrue(reset, std::string(f.name) + ": RST idles the IPU and empties both FIFOs");
                d.out.clear();
                d.feeder.pauseAt = kNoPause;
                std::vector<uint32_t> again;
                t.IsTrue(decodeSliceWithBdec(d, f, s, again) == Outcome::Done && d.out == cold.out,
                         std::string(f.name) + ": the slice decodes as on a fresh IPU");
            }
        });

        tc.Run("BDEC on randomly corrupted slices ends with whole macroblocks, an error or a wait, deterministically", [](TestCase &t)
        {
            uint32_t seed = 0x1234567u;
            size_t errors = 0u, cases = 0u, bad = 0u;
            for (const Fixture &f : ipu_mpeg_fixtures::kFixtures)
            {
                for (size_t si = 0; si < f.sliceCount; ++si)
                {
                    const Slice &s = f.slices[si];
                    for (int variant = 0; variant < 24; ++variant)
                    {
                        std::vector<uint8_t> bytes(s.data, s.data + s.size);
                        const uint32_t flips = 1u + nextRandom(seed) % 4u;
                        for (uint32_t k = 0; k < flips; ++k)
                        {
                            const uint32_t bit = s.headerBits + nextRandom(seed) % (static_cast<uint32_t>(s.size) * 8u - s.headerBits);
                            bytes[bit / 8u] ^= static_cast<uint8_t>(0x80u >> (bit % 8u));
                        }
                        Slice corrupt = s;
                        corrupt.data = bytes.data();
                        Driver first, second;
                        std::vector<uint32_t> c1, c2;
                        const Outcome o1 = decodeSliceWithBdec(first, f, corrupt, c1);
                        const Outcome o2 = decodeSliceWithBdec(second, f, corrupt, c2);
                        ++cases;
                        errors += o1 == Outcome::Error ? 1u : 0u;
                        const bool consistent = o1 == o2 && first.out == second.out &&
                                                first.out.size() == c1.size() * kRawBytes &&
                                                (o1 != Outcome::Error || !first.ipu.busy());
                        bad += consistent ? 0u : 1u;
                    }
                }
            }
            t.Equals(bad, static_cast<size_t>(0u), "every corrupted slice: same result twice, whole macroblocks, an idle IPU after an error");
            t.IsTrue(errors != 0u, "corruption is detected as a bitstream error in " + std::to_string(errors) + "/" + std::to_string(cases) + " cases");
        });
    });
}
