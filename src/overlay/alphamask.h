#pragma once

// Hit masks of the pets (section 3.4 of the phase 2 plan): a bit grid over a pet's picture, at most 128 x 128 cells, set where
// the picture has any pixel with alpha >= 16/255 (a little generous: never smaller than what is drawn). Pure, Qt free: the
// decoder feeds it pixels, the window turns it into an input region, the state machine asks it questions.
//
//   - AlphaMask      one grid; contains(), union, coarsening 128 -> 64 -> 32, rotation/flip, run-length rectangles.
//   - MaskSet        the masks of one pet: one per frame, the union of them, and how much quality the budget left.
//   - MaskBuilder    makes a MaskSet frame by frame inside a byte budget (8 MiB for all pets, section 3.4), lowering the grid and
//                    finally keeping only the union.
//   - the .mask file a small versioned format with a bounded parser; its bytes are never trusted.
//   - maskTest()     the adapter to PetInteraction's std::function hit test.

#include "petbehaviours.h"
#include "petgeometry.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace murasaki {

inline constexpr int kMaskAlphaThreshold = 16;       // alpha >= 16/255 is opaque for a hit test
inline constexpr int kMaskGridLevels[] = {128, 64, 32};
inline constexpr int kMaxMaskGrid = 128;
inline constexpr int kMaxMaskSpace = 16384;          // the largest picture side a mask describes
inline constexpr int kMaxMaskFrames = 20000;         // the frame cache's own limit (petlimits.h)
inline constexpr std::size_t kMaskBudgetBytes = std::size_t(8) << 20; // for all pets together; one pet gets what the controller gives
// Input regions: how many rectangles a region may have (task 0, U2/U3).
inline constexpr std::size_t kMaskOverheadBytes = 80; // sizeof(AlphaMask) (a vector, five ints) and a heap block header, rounded up
inline constexpr int kMaxFrameRects = 128; // replaced at up to 30 Hz: within the noise of one rectangle on KWin
inline constexpr int kMaxFixedRects = 512; // set once or rarely (the silhouette)

struct MaskRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool operator==(const MaskRect &) const = default;
};

class AlphaMask
{
public:
    AlphaMask() = default;
    // An empty mask (no cell set) for a picture of spaceW x spaceH pixels at a grid of at most `level` cells on the longest side.
    AlphaMask(int spaceW, int spaceH, int level);

    // From the alpha bytes of a picture: `alpha` points at the first pixel's alpha, `pixelStep` bytes apart (4 for RGBA8888 or
    // ARGB32 in memory, 1 for an alpha plane), `stride` bytes per row. Null or degenerate input gives an empty mask.
    static AlphaMask fromAlpha(const std::uint8_t *alpha, int stride, int pixelStep, int w, int h, int level = kMaxMaskGrid);

    bool isNull() const { return m_cols == 0; }
    int spaceWidth() const { return m_spaceW; }
    int spaceHeight() const { return m_spaceH; }
    int cols() const { return m_cols; }
    int rows() const { return m_rows; }
    int cellPx() const { return m_cell; }
    // The grid's longest side in cells: 128, 64 or 32 (or less for a small picture).
    int level() const { return m_cols > m_rows ? m_cols : m_rows; }
    // The bits alone, and what a mask costs in memory: the bits plus the object and a typical allocator header (what the budget counts).
    std::size_t payloadBytes() const { return m_words.size() * sizeof(std::uint64_t); }
    std::size_t bytes() const { return isNull() ? 0 : payloadBytes() + kMaskOverheadBytes; }

    bool cell(int col, int row) const;
    void setCell(int col, int row, bool on = true);
    // Is the pixel (x, y) of the picture on a set cell? Outside the picture: no.
    bool contains(double x, double y) const;
    int setCells() const;
    bool empty() const { return setCells() == 0; }

    // The union of two masks of the same shape (a different shape is ignored: returns *this).
    AlphaMask united(const AlphaMask &other) const;
    bool isSubsetOf(const AlphaMask &other) const;
    // Halves the grid: each cell is the OR of a 2 x 2 block, so the mask only grows. Stops at 32 cells (a picture with fewer
    // cells to begin with is not coarsened at all).
    AlphaMask coarsened() const;
    bool canCoarsen() const { return level() > kMaskGridLevels[2]; }

    // The picture turned by `degrees` (clockwise, about its centre) after being flipped in its own frame (petgeometry.h): a
    // mask of the footprint, conservative (a cell is set when any of 3 x 3 sample points of it falls on a set source cell).
    AlphaMask transformed(double degrees, bool flipH, bool flipV) const;

    // Run-length rectangles in picture pixels (none for an empty mask or maxRects < 1): each row's runs of set cells, merged vertically when the same run repeats. When
    // there are more than `maxRects`, the grid is lowered until it fits; a mask that still does not fit becomes its bounding
    // rectangle (a superset). `coarsenSteps` says how often the grid was lowered.
    struct Rects {
        std::vector<MaskRect> rects;
        int coarsenSteps = 0;
        bool boundingBox = false;
    };
    Rects toRects(int maxRects) const;

    bool operator==(const AlphaMask &) const = default;

    // Word access for the file format (row major, 64 cells a word, bit 0 is the left cell).
    const std::vector<std::uint64_t> &words() const { return m_words; }
    static std::size_t wordsFor(int cols, int rows) { return std::size_t(rows) * std::size_t((cols + 63) / 64); }
    // Rebuilds a mask from its words; nothing when they do not fit the shape or set a bit outside the grid.
    static std::optional<AlphaMask> fromWords(int spaceW, int spaceH, int cols, int rows, int cell, std::vector<std::uint64_t> words);

private:
    int wordsPerRow() const { return (m_cols + 63) / 64; }
    int m_spaceW = 0;
    int m_spaceH = 0;
    int m_cols = 0;
    int m_rows = 0;
    int m_cell = 1;
    std::vector<std::uint64_t> m_words;
};

// What the budget left of a pet's masks.
enum class MaskQuality {
    None,       // no mask (yet): the whole rectangle takes input
    Full,       // one mask per frame, 128 grid (or the picture's own size when smaller)
    Coarse,     // one mask per frame on a 64 or 32 grid
    Silhouette  // the union of all frames only
};

class MaskSet
{
public:
    MaskSet() = default;
    MaskQuality quality() const { return m_quality; }
    int frameCount() const { return int(m_frames.size()); }
    const AlphaMask &silhouette() const { return m_union; }
    // The mask of frame `index` (wrapped); the union when there are no per-frame masks.
    const AlphaMask &maskFor(int index) const;
    std::size_t bytes() const;
    bool isNull() const { return m_quality == MaskQuality::None; }
    // `hit mask: coarse` / `hit mask: silhouette` / `hit mask: none` (nothing for Full).
    std::optional<std::string_view> degradeName() const;
    MaskSet transformed(double degrees, bool flipH, bool flipV) const;

    // Assembles a set from parts the caller has checked (the builder, the parser). Quality None if there is nothing.
    static MaskSet assemble(MaskQuality quality, AlphaMask silhouette, std::vector<AlphaMask> frames);

private:
    MaskQuality m_quality = MaskQuality::None;
    AlphaMask m_union;
    std::vector<AlphaMask> m_frames;
};

// Builds a MaskSet frame by frame within `budgetBytes` (bytes() of the frames and the union together): when the frames still to
// come would not fit at the current grid, the grid is lowered (64, then 32), and when 32 does not fit either only the union is
// kept. The union, at the finest grid (at most about 2 KiB), is always kept, even when the budget is smaller than one mask: a pet
// then has `hit mask: silhouette` and the budget is exceeded by that one mask. The controller reserves for it.
class MaskBuilder
{
public:
    MaskBuilder(int spaceW, int spaceH, int expectedFrames, std::size_t budgetBytes = kMaskBudgetBytes);
    // Adds the next frame's alpha (see AlphaMask::fromAlpha); false when the frame does not match the picture size or the
    // builder was given up.
    bool addFrame(const std::uint8_t *alpha, int stride, int pixelStep, int w, int h);
    bool addMask(const AlphaMask &mask); // a frame mask of the finest grid, as AlphaMask::fromAlpha(..., 128) makes it
    MaskSet finish();
    // The grid of the frames now: the finest grid's longest side halved `steps` times.
    int level() const;
    bool unionOnly() const { return m_unionOnly; }

private:
    void fit();
    int m_w;
    int m_h;
    int m_expected;
    std::size_t m_budget;
    int m_steps = 0; // coarsenings of the 128 grid applied to the frames
    bool m_unionOnly = false;
    int m_added = 0;
    AlphaMask m_union;
    std::vector<AlphaMask> m_frames;
};

// ---- the .mask file ----------------------------------------------------------------------------------------------------
// little endian: "MSKM", u32 version (1), u32 spaceW, u32 spaceH, u32 frameCount, u8 quality (1 full, 2 coarse, 3 silhouette), u8 0,
// u16 cols, u16 rows, u16 cell, u32 0 (reserved), u64 FNV-1a of the body; the body is the union's words, then the frames' words. The size is
// exactly what the header says: a file that is short, long, over the bound, of another version or whose masks do not nest is
// refused before anything is allocated from its numbers.
inline constexpr std::uint32_t kMaskFileVersion = 1;
inline constexpr std::size_t kMaskFileHeaderBytes = 40;
inline constexpr std::size_t kMaxMaskFileBytes = kMaskBudgetBytes + kMaskFileHeaderBytes;

std::vector<std::uint8_t> serializeMaskFile(const MaskSet &set);
// The set in `data`, or nothing. Expected values <= 0 are not checked; a cache of another variant (other size or frame count)
// is refused when they are given. Silhouette-only files carry frameCount 0 and match any expected frame count.
std::optional<MaskSet> parseMaskFile(const std::uint8_t *data, std::size_t size, int expectedW = 0, int expectedH = 0,
                                     int expectedFrames = 0);
// Writes through a unique temporary file and an atomic rename; false when the set is empty, when the file would be over
// kMaxMaskFileBytes (readMaskFile would refuse it) or when any write, flush or the rename fails.
bool writeMaskFile(const std::string &path, const MaskSet &set);
std::optional<MaskSet> readMaskFile(const std::string &path, int expectedW = 0, int expectedH = 0, int expectedFrames = 0);

// ---- the adapter to PetInteraction's hit test -------------------------------------------------------------------------
// `window` is the size, in window pixels, that the mask's picture is shown at (the pet's window): window coordinates are
// scaled onto the mask's picture. `provider` is asked at every call, so the caller can swap the current frame's mask.
std::function<bool(PetPoint)> maskTest(std::function<std::shared_ptr<const AlphaMask>()> provider, PetSize window);

} // namespace murasaki
