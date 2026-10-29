#pragma once

#include <QList>
#include <QString>

namespace murasaki::import {

// Sprite sheet geometry (docs/pets/import.md). The fields and defaults are Anima Engine's: a frame of 64 x 64, 4 frames, counted from frame 1, padding
// at the left and top edge of the sheet, spacing between the cells, 100 ms a frame. Pure arithmetic, no image: the caller cuts the rectangles.
struct SheetSpec {
    int sheetWidth = 0;
    int sheetHeight = 0;
    int frameWidth = 64;
    int frameHeight = 64;
    int frameCount = 4;
    int startFrame = 1;      // 1-based
    int paddingLeft = 0;
    int paddingTop = 0;
    int spacingHorizontal = 0;
    int spacingVertical = 0;
    int columns = 0;         // 0: as many as fit; else at most that many (never more than fit)
    int rows = 0;            // 0: as many as fit
    int frameMs = 100;       // carried for the caller; not used by the arithmetic
};

struct SheetCell {
    int x = 0;
    int y = 0;
    bool operator==(const SheetCell &) const = default;
};

struct SheetLayout {
    QList<SheetCell> cells;  // in playing order; each is frameWidth x frameHeight
    int columns = 0;
    int rows = 0;
    QString error;           // a name of petimport.h (+ ": detail"); empty when the cells are valid
    bool ok() const { return error.isEmpty(); }
};

// Cell n (0-based, counting from startFrame) is the cell with index startFrame - 1 + n of a row-major grid; a cell that falls partly outside the sheet is
// an error, never a crop. All arithmetic is 64-bit and every size is checked first (the sheet against kMaxSheetSide and kMaxSheetPixels, the frame
// against the canvas limit, the count against kMaxFrames), so a hostile number cannot overflow or allocate.
SheetLayout sliceSheet(const SheetSpec &spec);

} // namespace murasaki::import
