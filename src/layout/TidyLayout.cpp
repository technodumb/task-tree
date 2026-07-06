#include "layout/TidyLayout.hpp"

#include <algorithm>
#include <limits>

namespace tt {
namespace {

// A subtree contour: for each relative depth, the left (min) and right (max) edge
// of the subtree, expressed in a frame where the subtree's root centre is at x = 0.
struct Contour {
    std::vector<float> left;
    std::vector<float> right;
};

constexpr float kBig = std::numeric_limits<float>::max();

// State carried through the recursive walk.
struct Walker {
    const Forest& forest;
    const std::unordered_map<TaskId, Size>& sizes;
    const LayoutParams& p;
    std::unordered_map<TaskId, float> relX; // node centre x relative to its parent's centre

    Size sizeOf(TaskId id) const {
        auto it = sizes.find(id);
        return it != sizes.end() ? it->second : p.defaultSize;
    }

    // Place a row of sibling subtrees (given their contours) left to right so no two
    // overlap by less than hGap, and return each subtree's root-centre x in the row's
    // local frame (first root sits wherever the packing puts it, starting at 0).
    std::vector<float> placeSiblings(const std::vector<Contour>& cs) const {
        std::vector<float> xs(cs.size(), 0.f);
        std::vector<float> accRight; // rightmost edge so far, per depth
        for (std::size_t i = 0; i < cs.size(); ++i) {
            float offset = 0.f;
            if (i > 0) {
                const std::size_t od = std::min(accRight.size(), cs[i].left.size());
                for (std::size_t d = 0; d < od; ++d) {
                    // Need: incoming left edge >= accumulated right edge + hGap.
                    offset = std::max(offset, accRight[d] + p.hGap - cs[i].left[d]);
                }
            }
            xs[i] = offset;
            for (std::size_t d = 0; d < cs[i].right.size(); ++d) {
                const float r = cs[i].right[d] + offset;
                if (d < accRight.size()) accRight[d] = std::max(accRight[d], r);
                else                     accRight.push_back(r);
            }
        }
        return xs;
    }

    // First walk (post-order): position each node's children, record their relX, and
    // return this subtree's contour in a frame centred on this node.
    Contour firstWalk(TaskId id) {
        const Size sz = sizeOf(id);
        const float hw = sz.w * 0.5f;
        const Task* t = forest.get(id);
        const std::vector<TaskId>& kids = t ? t->children : std::vector<TaskId>{};

        if (kids.empty()) {
            return Contour{{-hw}, {hw}};
        }

        std::vector<Contour> childContours;
        childContours.reserve(kids.size());
        for (TaskId c : kids) childContours.push_back(firstWalk(c));

        const std::vector<float> xs = placeSiblings(childContours);
        const float rootCentre = (xs.front() + xs.back()) * 0.5f;
        for (std::size_t i = 0; i < kids.size(); ++i) relX[kids[i]] = xs[i] - rootCentre;

        // Build this node's contour: depth 0 is the node box itself; deeper depths
        // come from the children shifted into this node's centred frame.
        Contour out;
        out.left.push_back(-hw);
        out.right.push_back(hw);
        std::size_t maxD = 0;
        for (const Contour& cc : childContours) maxD = std::max(maxD, cc.left.size());
        for (std::size_t d = 0; d < maxD; ++d) {
            float lmin = kBig, rmax = -kBig;
            for (std::size_t i = 0; i < kids.size(); ++i) {
                if (d < childContours[i].left.size()) {
                    const float shift = relX[kids[i]];
                    lmin = std::min(lmin, childContours[i].left[d] + shift);
                    rmax = std::max(rmax, childContours[i].right[d] + shift);
                }
            }
            out.left.push_back(lmin);
            out.right.push_back(rmax);
        }
        return out;
    }
};

} // namespace

std::unordered_map<TaskId, Rect> computeLayout(const Forest& forest,
                                               const std::unordered_map<TaskId, Size>& sizes,
                                               const LayoutParams& params) {
    std::unordered_map<TaskId, Rect> out;
    if (forest.roots.empty()) return out;

    Walker w{forest, sizes, params, {}};

    // First walk each root (fills relX for all descendants) then pack the roots as
    // siblings of an implicit super-root centred at x = 0.
    std::vector<Contour> rootContours;
    rootContours.reserve(forest.roots.size());
    for (TaskId r : forest.roots) rootContours.push_back(w.firstWalk(r));
    const std::vector<float> rootXs = w.placeSiblings(rootContours);
    const float superCentre = (rootXs.front() + rootXs.back()) * 0.5f;
    for (std::size_t i = 0; i < forest.roots.size(); ++i)
        w.relX[forest.roots[i]] = rootXs[i] - superCentre;

    // Assign depth and absolute centre-x to every node (pre-order DFS), and gather
    // the max node height on each layer.
    std::unordered_map<TaskId, int> depth;
    std::unordered_map<TaskId, float> absX;
    std::vector<float> layerMaxH;

    struct Frame { TaskId id; int d; float parentAbsX; };
    std::vector<Frame> stack;
    for (auto it = forest.roots.rbegin(); it != forest.roots.rend(); ++it)
        stack.push_back({*it, 0, 0.f});

    while (!stack.empty()) {
        const Frame f = stack.back();
        stack.pop_back();
        const float ax = f.parentAbsX + w.relX[f.id];
        absX[f.id] = ax;
        depth[f.id] = f.d;

        const Size sz = w.sizeOf(f.id);
        if (static_cast<int>(layerMaxH.size()) <= f.d) layerMaxH.resize(f.d + 1, 0.f);
        layerMaxH[f.d] = std::max(layerMaxH[f.d], sz.h);

        if (const Task* t = forest.get(f.id)) {
            for (auto it = t->children.rbegin(); it != t->children.rend(); ++it)
                stack.push_back({*it, f.d + 1, ax});
        }
    }

    // Per-layer top y: each layer starts below the tallest node of the layer above.
    std::vector<float> layerY(layerMaxH.size(), 0.f);
    if (!layerY.empty()) layerY[0] = params.topMargin;
    for (std::size_t d = 1; d < layerY.size(); ++d)
        layerY[d] = layerY[d - 1] + layerMaxH[d - 1] + params.vGap;

    // Horizontal placement: centre the forest within centerWidth when it fits,
    // otherwise left-align at leftMargin.
    float minLeft = kBig, maxRight = -kBig;
    for (const auto& [id, ax] : absX) {
        const float hw = w.sizeOf(id).w * 0.5f;
        minLeft = std::min(minLeft, ax - hw);
        maxRight = std::max(maxRight, ax + hw);
    }
    float targetLeft = params.leftMargin;
    if (params.centerWidth > 0.f) {
        const float treeW = maxRight - minLeft;
        targetLeft = std::max(params.leftMargin, (params.centerWidth - treeW) * 0.5f);
    }
    const float shiftX = targetLeft - minLeft;

    out.reserve(absX.size());
    for (const auto& [id, ax] : absX) {
        const Size sz = w.sizeOf(id);
        out[id] = Rect{ax - sz.w * 0.5f + shiftX, layerY[depth[id]], sz.w, sz.h};
    }
    return out;
}

} // namespace tt
