#pragma once

class GfxRenderer;
class MappedInputManager;

// Fork additions for frontlight boards (X4 Pro), active only on the reader's
// page view. The edge-slide idea and its distance scale follow CrossInk
// (https://github.com/uxjulia/crossink, MIT).
namespace readerlight {

// Vertical slide along the left screen edge = brightness, right edge = warmth
// (up = more). Returns true while a slide owns the touch input, in which case
// the reader must skip the rest of its input handling for this pass.
bool handleEdgeSlide(const MappedInputManager& input, const GfxRenderer& renderer);

}  // namespace readerlight
