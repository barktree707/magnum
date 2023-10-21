/*
    This file is part of Magnum.

    Copyright © 2010, 2011, 2012, 2013, 2014, 2015, 2016, 2017, 2018, 2019,
                2020, 2021, 2022, 2023 Vladimír Vondruš <mosra@centrum.cz>

    Permission is hereby granted, free of charge, to any person obtaining a
    copy of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom the
    Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.
*/

#include "Renderer.h"

#include <Corrade/Containers/StridedArrayView.h>
#include <Corrade/Containers/Triple.h>

#include "Magnum/Math/Functions.h"
#include "Magnum/Text/AbstractFont.h"
#include "Magnum/Text/AbstractGlyphCache.h"
#include "Magnum/Text/Direction.h"

#ifdef MAGNUM_TARGET_GL
#include <Corrade/Containers/Array.h>
#include <Corrade/Containers/ArrayViewStl.h> /** @todo remove once Renderer is STL-free */
#include <Corrade/Containers/StringStl.h> /** @todo remove once Renderer is STL-free */

#include "Magnum/Mesh.h"
#include "Magnum/GL/Context.h"
#include "Magnum/GL/Extensions.h"
#include "Magnum/GL/Mesh.h"
#include "Magnum/Shaders/GenericGL.h"
#include "Magnum/Text/AbstractShaper.h"
#endif

namespace Magnum { namespace Text {

Range2D renderLineGlyphPositionsInto(const AbstractFont& font, const Float size, const LayoutDirection direction, const Containers::StridedArrayView1D<const Vector2>& glyphOffsets, const Containers::StridedArrayView1D<const Vector2>& glyphAdvances, Vector2& cursor, const Containers::StridedArrayView1D<Vector2>& glyphPositions) {
    CORRADE_ASSERT(glyphAdvances.size() == glyphOffsets.size() &&
                   glyphPositions.size() == glyphOffsets.size(),
        "Text::renderLineGlyphPositionsInto(): expected glyphOffsets, glyphAdvances and output views to have the same size, got" << glyphOffsets.size() << Debug::nospace << "," << glyphAdvances.size() << "and" << glyphPositions.size(), {});
    CORRADE_ASSERT(direction == LayoutDirection::HorizontalTopToBottom,
        "Text::renderLineGlyphPositionsInto(): only" << LayoutDirection::HorizontalTopToBottom << "is supported right now, got" << direction, {});
    #ifdef CORRADE_NO_ASSERT
    static_cast<void>(direction); /** @todo drop once implemented */
    #endif

    CORRADE_ASSERT(font.isOpened(),
        "Text::renderLineGlyphPositionsInto(): no font opened", {});
    const Float scale = size/font.size();

    /* Combine the offsets and cursor advances and calculate the line rectangle
       along the way. Initially the cursor is at origin and rectangle is empty,
       with just the Y bounds from font metrics. */
    Range2D rectangle{cursor + Vector2::yAxis(font.descent()*scale),
                      cursor + Vector2::yAxis(font.ascent()*scale)};
    for(UnsignedInt i = 0; i != glyphOffsets.size(); ++i) {
        /* The glyphOffsets and output are allowed to be aliased, so make sure
           the value isn't stomped on when writing the output */
        glyphPositions[i] = cursor + glyphOffsets[i]*scale;
        cursor += glyphAdvances[i]*scale;

        /* Extend the line rectangle with the cursor range */
        /** @todo this assumes left-to-right direction, update when vertical
            and LTR text is possible & testable */
        rectangle.max() = Math::max(rectangle.max(), cursor);
    }

    return rectangle;
}

namespace {

Range2D renderGlyphQuadsInto(const AbstractFont& font, const Float size, const AbstractGlyphCache& cache, const Containers::StridedArrayView1D<const Vector2>& glyphPositions, const Containers::StridedArrayView1D<const UnsignedInt>& glyphIds, const Containers::StridedArrayView1D<Vector2>& vertexPositions, const Containers::StridedArrayView1D<Vector2>& vertexTextureCoordinates, const Containers::StridedArrayView1D<Float>& vertexTextureLayers) {
    CORRADE_ASSERT(glyphIds.size() == glyphPositions.size(),
        "Text::renderGlyphQuadsInto(): expected glyphIds and glyphPositions views to have the same size, got" << glyphIds.size() << "and" << glyphPositions.size(), {});
    CORRADE_ASSERT(vertexPositions.size() == glyphPositions.size()*4 &&
                   vertexTextureCoordinates.size() == glyphPositions.size()*4,
        "Text::renderGlyphQuadsInto(): expected vertexPositions and vertexTextureCoordinates views to have" << glyphPositions.size()*4 << "elements, got" << vertexPositions.size() << "and" << vertexTextureCoordinates.size(), {});
    /* Should be ensured by the callers below */
    CORRADE_INTERNAL_ASSERT(!vertexTextureLayers || vertexTextureLayers.size() == vertexTextureCoordinates.size());

    CORRADE_ASSERT(font.isOpened(),
        "Text::renderGlyphQuadsInto(): no font opened", {});
    const Float scale = size/font.size();
    const Vector2 inverseCacheSize = 1.0f/Vector2{cache.size().xy()};

    const Containers::Optional<UnsignedInt> fontId = cache.findFont(&font);
    CORRADE_ASSERT(fontId,
        "Text::renderGlyphQuadsInto(): font not found among" << cache.fontCount() << "fonts in passed glyph cache", {});

    /* Get all glyphs from the glyph cache, create quads for each and calculate
       the glyph bound rectangle along the way. */
    Range2D rectangle;
    for(std::size_t i = 0; i != glyphIds.size(); ++i) {
        /* Offset of the glyph rectangle relative to the cursor, layer,
           texture coordinates. We checked that the glyph cache is 2D above
           so the layer can be ignored. */
        const Containers::Triple<Vector2i, Int, Range2Di> cacheGlyph = cache.glyph(*fontId, glyphIds[i]);

        /* 2---3
           |   |
           |   |
           |   |
           0---1 */
        const Range2D quad = Range2D::fromSize(
            glyphPositions[i] + Vector2{cacheGlyph.first()}*scale,
            Vector2{cacheGlyph.third().size()}*scale);
        const Range2D texture = Range2D{cacheGlyph.third()}
            .scaled(inverseCacheSize);
        const std::size_t i4 = i*4;
        for(UnsignedByte j = 0; j != 4; ++j) {
            /* ✨ */
            vertexPositions[i4 + j] = Math::lerp(quad.min(), quad.max(), BitVector2{j});
            vertexTextureCoordinates[i4 + j] = Math::lerp(texture.min(), texture.max(), BitVector2{j});
        }

        /* Fill also a texture layer if desirable. For 2D output the caller
           already checked that the cache is 2D. */
        if(vertexTextureLayers) for(std::size_t j = 0; j != 4; ++j)
            vertexTextureLayers[i4 + j] = cacheGlyph.second();

        /* Extend the rectangle with current glyph bounds */
        rectangle = Math::join(rectangle, quad);
    }

    return rectangle;
}

}

Range2D renderGlyphQuadsInto(const AbstractFont& font, const Float size, const AbstractGlyphCache& cache, const Containers::StridedArrayView1D<const Vector2>& glyphPositions, const Containers::StridedArrayView1D<const UnsignedInt>& glyphIds, const Containers::StridedArrayView1D<Vector2>& vertexPositions, const Containers::StridedArrayView1D<Vector3>& vertexTextureCoordinates) {
    return renderGlyphQuadsInto(font, size, cache, glyphPositions, glyphIds, vertexPositions, vertexTextureCoordinates.slice(&Vector3::xy), vertexTextureCoordinates.slice(&Vector3::z));
}

Range2D renderGlyphQuadsInto(const AbstractFont& font, const Float size, const AbstractGlyphCache& cache, const Containers::StridedArrayView1D<const Vector2>& glyphPositions, const Containers::StridedArrayView1D<const UnsignedInt>& glyphIds, const Containers::StridedArrayView1D<Vector2>& vertexPositions, const Containers::StridedArrayView1D<Vector2>& vertexTextureCoordinates) {
    CORRADE_ASSERT(cache.size().z() == 1,
        "Text::renderGlyphQuadsInto(): can't use this overload with an array glyph cache", {});
    return renderGlyphQuadsInto(font, size, cache, glyphPositions, glyphIds, vertexPositions, vertexTextureCoordinates, nullptr);
}

Range2D alignRenderedLine(const Range2D& lineRectangle, const LayoutDirection direction, const Alignment alignment, const Containers::StridedArrayView1D<Vector2>& positions) {
    CORRADE_ASSERT(direction == LayoutDirection::HorizontalTopToBottom,
        "Text::alignRenderedLine(): only" << LayoutDirection::HorizontalTopToBottom << "is supported right now, got" << direction, {});
    #ifdef CORRADE_NO_ASSERT
    static_cast<void>(direction); /** @todo drop once implemented */
    #endif

    /** @todo this again assumes horizontal direction, needs to be updated once
        vertical (and possibly mixed horizontal/vertical) text is possible */

    Float alignmentOffsetX = 0.0f;
    if((UnsignedByte(alignment) & Implementation::AlignmentHorizontal) == Implementation::AlignmentLeft)
        alignmentOffsetX = -lineRectangle.left();
    else if((UnsignedByte(alignment) & Implementation::AlignmentHorizontal) == Implementation::AlignmentCenter) {
        alignmentOffsetX = -lineRectangle.centerX();
        /* Integer alignment */
        if(UnsignedByte(alignment) & Implementation::AlignmentIntegral)
            alignmentOffsetX = Math::round(alignmentOffsetX);
    }
    else if((UnsignedByte(alignment) & Implementation::AlignmentHorizontal) == Implementation::AlignmentRight)
        alignmentOffsetX = -lineRectangle.right();

    /* Shift all positions */
    for(Vector2& i: positions)
        i.x() += alignmentOffsetX;

    return lineRectangle.translated(Vector2::xAxis(alignmentOffsetX));
}

Range2D alignRenderedBlock(const Range2D& blockRectangle, const LayoutDirection direction, const Alignment alignment, const Containers::StridedArrayView1D<Vector2>& positions) {
    CORRADE_ASSERT(direction == LayoutDirection::HorizontalTopToBottom,
        "Text::alignRenderedBlock(): only" << LayoutDirection::HorizontalTopToBottom << "is supported right now, got" << direction, {});
    #ifdef CORRADE_NO_ASSERT
    static_cast<void>(direction); /** @todo drop once implemented */
    #endif

    /** @todo this assumes vertical layout advance, needs to be updated once
        other directions are possible */

    Float alignmentOffsetY = 0.0f;
    if((UnsignedByte(alignment) & Implementation::AlignmentVertical) == Implementation::AlignmentBottom)
        alignmentOffsetY = -blockRectangle.bottom();
    else if((UnsignedByte(alignment) & Implementation::AlignmentVertical) == Implementation::AlignmentMiddle) {
        alignmentOffsetY = -blockRectangle.centerY();
        /* Integer alignment */
        if(UnsignedByte(alignment) & Implementation::AlignmentIntegral)
            alignmentOffsetY = Math::round(alignmentOffsetY);
    }
    else if((UnsignedByte(alignment) & Implementation::AlignmentVertical) == Implementation::AlignmentTop)
        alignmentOffsetY = -blockRectangle.top();

    /* Shift all positions */
    for(Vector2& i: positions)
        i.y() += alignmentOffsetY;

    return blockRectangle.translated(Vector2::yAxis(alignmentOffsetY));
}

namespace {

template<class T> void renderGlyphQuadIndicesIntoInternal(const UnsignedInt glyphOffset, const Containers::StridedArrayView1D<T>& indices) {
    CORRADE_ASSERT(indices.size() % 6 == 0,
        "Text::renderGlyphQuadIndicesInto(): expected the indices view size to be divisible by 6, got" << indices.size(), );
    const UnsignedInt glyphCount = indices.size()/6;
    #ifndef CORRADE_NO_ASSERT
    const UnsignedLong maxValue = UnsignedLong(glyphOffset)*4 + UnsignedLong(glyphCount)*4;
    #endif
    CORRADE_ASSERT(maxValue <= (1ull << 8*sizeof(T)),
        "Text::renderGlyphQuadIndicesInto(): max index value of" << maxValue - 1 << "cannot fit into a" << 8*sizeof(T) << Debug::nospace << "-bit type", );

    for(UnsignedInt i = 0; i != glyphCount; ++i) {
        /* 2---3 2 3---5
           |   | |\ \  |
           |   | | \ \ |
           |   | |  \ \|
           0---1 0---1 4 */
        const UnsignedInt i4 = (glyphOffset + i)*4;
        const UnsignedInt i6 = i*6;
        indices[i6 + 0] = i4 + 0;
        indices[i6 + 1] = i4 + 1;
        indices[i6 + 2] = i4 + 2;
        indices[i6 + 3] = i4 + 2;
        indices[i6 + 4] = i4 + 1;
        indices[i6 + 5] = i4 + 3;
    }
}

}

void renderGlyphQuadIndicesInto(UnsignedInt glyphOffset, const Containers::StridedArrayView1D<UnsignedInt>& indices) {
    renderGlyphQuadIndicesIntoInternal(glyphOffset, indices);
}

void renderGlyphQuadIndicesInto(UnsignedInt glyphOffset, const Containers::StridedArrayView1D<UnsignedShort>& indices) {
    renderGlyphQuadIndicesIntoInternal(glyphOffset, indices);
}

void renderGlyphQuadIndicesInto(UnsignedInt glyphOffset, const Containers::StridedArrayView1D<UnsignedByte>& indices) {
    renderGlyphQuadIndicesIntoInternal(glyphOffset, indices);
}

#ifdef MAGNUM_TARGET_GL
namespace {

struct Vertex {
    Vector2 position, textureCoordinates;
};

std::tuple<std::vector<Vertex>, Range2D> renderVerticesInternal(AbstractFont& font, const AbstractGlyphCache& cache, const Float size, const std::string& text, const Alignment alignment) {
    /* This was originally added as a runtime error into plugin implementations
       during the transition period for the new AbstractGlyphCache API, now
       it's an assert in the transition period for the Renderer API. Shouldn't
       get triggered by existing code that uses 2D caches. */
    CORRADE_ASSERT(cache.size().z() == 1,
        "Text::Renderer: array glyph caches are not supported", {});

    /* Find this font in the cache and assert in the high-level API already to
       avoid confusion */
    CORRADE_ASSERT(cache.findFont(&font),
        "Text::Renderer: font not found among" << cache.fontCount() << "fonts in passed glyph cache", {});

    /* Output data, reserve memory as when the text would be ASCII-only. In
       reality the actual vertex count will be smaller, but allocating more at
       once is better than reallocating many times later. */
    std::vector<Vertex> vertices;
    vertices.reserve(text.size()*4);

    /* Scaling factor, line advance, total rendered bounds, initial line
       position, last+1 vertex on previous line */
    const Float scale = size/font.size();
    const Vector2 lineAdvance = Vector2::yAxis(font.lineHeight()*scale);
    Range2D rectangle;
    Vector2 linePosition;

    /* Temp buffer so we don't allocate for each new line */
    /**
     * @todo C++1z: use std::string_view to avoid the one allocation and all
     *      the copying altogether
     */
    std::string line;
    line.reserve(text.size());
    struct Glyph {
        UnsignedInt id;
        Vector2 offset;
        Vector2 advance;
    };
    Containers::Array<Glyph> glyphs{NoInit, text.size()};

    /* Create a shaper */
    /** @todo even with reusing a shaper this is all horrific, rework!! */
    Containers::Pointer<AbstractShaper> shaper = font.createShaper();

    /* Render each line separately and align it horizontally */
    std::size_t pos, prevPos = 0;
    do {
        /* Empty line, nothing to do (the rest is done below in while expression) */
        if((pos = text.find('\n', prevPos)) == prevPos) continue;

        /* Copy the line into the temp buffer */
        line.assign(text, prevPos, pos-prevPos);

        /* Shape the line, get the results */
        shaper->shape(line);
        const Containers::StridedArrayView1D<Glyph> lineGlyphs = glyphs.prefix(shaper->glyphCount());
        shaper->glyphIdsInto(
            lineGlyphs.slice(&Glyph::id));
        shaper->glyphOffsetsAdvancesInto(
            lineGlyphs.slice(&Glyph::offset),
            lineGlyphs.slice(&Glyph::advance));

        /* Verify that we don't reallocate anything. The only problem might
           arise when the layouter decides to compose one character from more
           than one glyph (i.e. accents). Will remove the asserts when this
           issue arises. */
        CORRADE_INTERNAL_ASSERT(vertices.size() + shaper->glyphCount()*4 <= vertices.capacity());

        Vector2 cursor = linePosition;

        /* Render line glyph positions into the first vertex of each quad in
           the output */
        vertices.resize(vertices.size() + shaper->glyphCount()*4);
        const Containers::StridedArrayView1D<Vertex> lineVertices = Containers::stridedArrayView(vertices).exceptPrefix(vertices.size() - shaper->glyphCount()*4);
        const Range2D lineRectangle = renderLineGlyphPositionsInto(
            font,
            size,
            /** @todo direction hardcoded here */
            LayoutDirection::HorizontalTopToBottom,
            lineGlyphs.slice(&Glyph::offset),
            lineGlyphs.slice(&Glyph::advance),
            cursor,
            lineVertices.slice(&Vertex::position).every(4));

        /* Create quads from the positions */
        const Range2D lineQuadRectangle = renderGlyphQuadsInto(
            font,
            size,
            cache,
            lineVertices.slice(&Vertex::position).every(4),
            lineGlyphs.slice(&Glyph::id),
            lineVertices.slice(&Vertex::position),
            lineVertices.slice(&Vertex::textureCoordinates));

        /* Horizontally align the line, using either of the rectangles based on
           which alignment is desired */
        const Range2D alignedLineRectangle = alignRenderedLine(
            UnsignedByte(alignment) & Implementation::AlignmentGlyphBounds ?
                lineQuadRectangle : lineRectangle,
            /** @todo direction hardcoded here */
            LayoutDirection::HorizontalTopToBottom,
            alignment,
            lineVertices.slice(&Vertex::position));

        /* Extend the rectangle with final line bounds */
        rectangle = Math::join(rectangle, alignedLineRectangle);

    /* Move to next line */
    } while(prevPos = pos+1,
            linePosition -= lineAdvance,
            pos != std::string::npos);

    const Range2D alignedRectangle = alignRenderedBlock(
        rectangle,
        /** @todo direction hardcoded here */
        LayoutDirection::HorizontalTopToBottom,
        alignment,
        Containers::stridedArrayView(vertices).slice(&Vertex::position));

    return std::make_tuple(Utility::move(vertices), alignedRectangle);
}

std::pair<Containers::Array<char>, MeshIndexType> renderIndicesInternal(const UnsignedInt glyphCount) {
    const UnsignedInt vertexCount = glyphCount*4;
    const UnsignedInt indexCount = glyphCount*6;

    Containers::Array<char> indices;
    MeshIndexType indexType;
    if(vertexCount <= 256) {
        indexType = MeshIndexType::UnsignedByte;
        indices = Containers::Array<char>{NoInit, indexCount*sizeof(UnsignedByte)};
        renderGlyphQuadIndicesInto(0, Containers::arrayCast<UnsignedByte>(indices));
    } else if(vertexCount <= 65536) {
        indexType = MeshIndexType::UnsignedShort;
        indices = Containers::Array<char>{NoInit, indexCount*sizeof(UnsignedShort)};
        renderGlyphQuadIndicesInto(0, Containers::arrayCast<UnsignedShort>(indices));
    } else {
        indexType = MeshIndexType::UnsignedInt;
        indices = Containers::Array<char>{NoInit, indexCount*sizeof(UnsignedInt)};
        renderGlyphQuadIndicesInto(0, Containers::arrayCast<UnsignedInt>(indices));
    }

    return {Utility::move(indices), indexType};
}

std::tuple<GL::Mesh, Range2D> renderInternal(AbstractFont& font, const AbstractGlyphCache& cache, Float size, const std::string& text, GL::Buffer& vertexBuffer, GL::Buffer& indexBuffer, GL::BufferUsage usage, Alignment alignment) {
    /* Render vertices and upload them */
    std::vector<Vertex> vertices;
    Range2D rectangle;
    std::tie(vertices, rectangle) = renderVerticesInternal(font, cache, size, text, alignment);
    vertexBuffer.setData(vertices, usage);

    const UnsignedInt glyphCount = vertices.size()/4;
    const UnsignedInt indexCount = glyphCount*6;

    /* Render indices and upload them */
    Containers::Array<char> indices;
    MeshIndexType indexType;
    std::tie(indices, indexType) = renderIndicesInternal(glyphCount);
    indexBuffer.setData(indices, usage);

    /* Configure mesh except for vertex buffer (depends on dimension count, done
       in subclass) */
    GL::Mesh mesh;
    mesh.setPrimitive(MeshPrimitive::Triangles)
        .setCount(indexCount)
        .setIndexBuffer(indexBuffer, 0, indexType, 0, vertices.size());

    return std::make_tuple(Utility::move(mesh), rectangle);
}

}

std::tuple<std::vector<Vector2>, std::vector<Vector2>, std::vector<UnsignedInt>, Range2D> AbstractRenderer::render(AbstractFont& font, const AbstractGlyphCache& cache, Float size, const std::string& text, Alignment alignment) {
    /* Render vertices */
    std::vector<Vertex> vertices;
    Range2D rectangle;
    std::tie(vertices, rectangle) = renderVerticesInternal(font, cache, size, text, alignment);

    /* Deinterleave the vertices */
    std::vector<Vector2> positions, textureCoordinates;
    positions.reserve(vertices.size());
    positions.reserve(textureCoordinates.size());
    for(const auto& v: vertices) {
        positions.push_back(v.position);
        textureCoordinates.push_back(v.textureCoordinates);
    }

    /* Render indices */
    const UnsignedInt glyphCount = vertices.size()/4;
    std::vector<UnsignedInt> indices(glyphCount*6);
    renderGlyphQuadIndicesInto(0, indices);

    return std::make_tuple(Utility::move(positions), Utility::move(textureCoordinates), Utility::move(indices), rectangle);
}

template<UnsignedInt dimensions> std::tuple<GL::Mesh, Range2D> Renderer<dimensions>::render(AbstractFont& font, const AbstractGlyphCache& cache, Float size, const std::string& text, GL::Buffer& vertexBuffer, GL::Buffer& indexBuffer, GL::BufferUsage usage, Alignment alignment) {
    /* Finalize mesh configuration and return the result */
    auto r = renderInternal(font, cache, size, text, vertexBuffer, indexBuffer, usage, alignment);
    GL::Mesh& mesh = std::get<0>(r);
    mesh.addVertexBuffer(vertexBuffer, 0,
        typename Shaders::GenericGL<dimensions>::Position(
            Shaders::GenericGL<dimensions>::Position::Components::Two),
        typename Shaders::GenericGL<dimensions>::TextureCoordinates());
    return r;
}

#if defined(MAGNUM_TARGET_GLES2) && !defined(CORRADE_TARGET_EMSCRIPTEN)
AbstractRenderer::BufferMapImplementation AbstractRenderer::bufferMapImplementation = &AbstractRenderer::bufferMapImplementationFull;
AbstractRenderer::BufferUnmapImplementation AbstractRenderer::bufferUnmapImplementation = &AbstractRenderer::bufferUnmapImplementationDefault;

void* AbstractRenderer::bufferMapImplementationFull(GL::Buffer& buffer, GLsizeiptr) {
    return buffer.map(GL::Buffer::MapAccess::WriteOnly);
}
#endif

#if !defined(MAGNUM_TARGET_GLES2) || defined(CORRADE_TARGET_EMSCRIPTEN)
inline void* AbstractRenderer::bufferMapImplementation(GL::Buffer& buffer, GLsizeiptr length)
#else
void* AbstractRenderer::bufferMapImplementationRange(GL::Buffer& buffer, GLsizeiptr length)
#endif
{
    #ifndef CORRADE_TARGET_EMSCRIPTEN
    return buffer.map(0, length, GL::Buffer::MapFlag::InvalidateBuffer|GL::Buffer::MapFlag::Write);
    #else
    static_cast<void>(length);
    return &buffer == &_indexBuffer ? _indexBufferData : _vertexBufferData;
    #endif
}

#if !defined(MAGNUM_TARGET_GLES2) || defined(CORRADE_TARGET_EMSCRIPTEN)
inline void AbstractRenderer::bufferUnmapImplementation(GL::Buffer& buffer)
#else
void AbstractRenderer::bufferUnmapImplementationDefault(GL::Buffer& buffer)
#endif
{
    #ifndef CORRADE_TARGET_EMSCRIPTEN
    buffer.unmap();
    #else
    buffer.setSubData(0, &buffer == &_indexBuffer ? _indexBufferData : _vertexBufferData);
    #endif
}

AbstractRenderer::AbstractRenderer(AbstractFont& font, const AbstractGlyphCache& cache, const Float size, const Alignment alignment): _vertexBuffer{GL::Buffer::TargetHint::Array}, _indexBuffer{GL::Buffer::TargetHint::ElementArray}, font(font), cache(cache), _fontSize{size}, _alignment(alignment), _capacity(0) {
    #ifndef MAGNUM_TARGET_GLES
    MAGNUM_ASSERT_GL_EXTENSION_SUPPORTED(GL::Extensions::ARB::map_buffer_range);
    #elif defined(MAGNUM_TARGET_GLES2) && !defined(CORRADE_TARGET_EMSCRIPTEN)
    if(GL::Context::current().isExtensionSupported<GL::Extensions::EXT::map_buffer_range>()) {
        bufferMapImplementation = &AbstractRenderer::bufferMapImplementationRange;
    } else {
        MAGNUM_ASSERT_GL_EXTENSION_SUPPORTED(GL::Extensions::OES::mapbuffer);
        Warning() << "Text::Renderer:" << GL::Extensions::EXT::map_buffer_range::string()
                  << "is not supported, using inefficient" << GL::Extensions::OES::mapbuffer::string()
                  << "instead";
    }
    #endif

    /* Vertex buffer configuration depends on dimension count, done in subclass */
    _mesh.setPrimitive(MeshPrimitive::Triangles);
}

AbstractRenderer::~AbstractRenderer() = default;

template<UnsignedInt dimensions> Renderer<dimensions>::Renderer(AbstractFont& font, const AbstractGlyphCache& cache, const Float size, const Alignment alignment): AbstractRenderer(font, cache, size, alignment) {
    /* Finalize mesh configuration */
    _mesh.addVertexBuffer(_vertexBuffer, 0,
        typename Shaders::GenericGL<dimensions>::Position(
            Shaders::GenericGL<dimensions>::Position::Components::Two),
        typename Shaders::GenericGL<dimensions>::TextureCoordinates());
}

void AbstractRenderer::reserve(const uint32_t glyphCount, const GL::BufferUsage vertexBufferUsage, const GL::BufferUsage indexBufferUsage) {
    _capacity = glyphCount;

    const UnsignedInt vertexCount = glyphCount*4;

    /* Allocate vertex buffer, reset vertex count */
    _vertexBuffer.setData({nullptr, vertexCount*sizeof(Vertex)}, vertexBufferUsage);
    #ifdef CORRADE_TARGET_EMSCRIPTEN
    _vertexBufferData = Containers::Array<UnsignedByte>(vertexCount*sizeof(Vertex));
    #endif
    _mesh.setCount(0);

    /* Render indices */
    Containers::Array<char> indexData;
    MeshIndexType indexType;
    std::tie(indexData, indexType) = renderIndicesInternal(glyphCount);

    /* Allocate index buffer, reset index count and reconfigure buffer binding */
    _indexBuffer.setData({nullptr, indexData.size()}, indexBufferUsage);
    #ifdef CORRADE_TARGET_EMSCRIPTEN
    _indexBufferData = Containers::Array<UnsignedByte>(indexData.size());
    #endif
    _mesh.setCount(0)
        .setIndexBuffer(_indexBuffer, 0, indexType, 0, vertexCount);

    /* Prefill index buffer */
    char* const indices = static_cast<char*>(bufferMapImplementation(_indexBuffer, indexData.size()));
    CORRADE_INTERNAL_ASSERT(indices);
    /** @todo Emscripten: it can be done without this copying altogether */
    std::copy(indexData.begin(), indexData.end(), indices);
    bufferUnmapImplementation(_indexBuffer);
}

void AbstractRenderer::render(const std::string& text) {
    /* Render vertex data */
    std::vector<Vertex> vertexData;
    _rectangle = {};
    std::tie(vertexData, _rectangle) = renderVerticesInternal(font, cache, _fontSize, text, _alignment);

    const UnsignedInt glyphCount = vertexData.size()/4;
    const UnsignedInt vertexCount = glyphCount*4;
    const UnsignedInt indexCount = glyphCount*6;

    CORRADE_ASSERT(glyphCount <= _capacity,
        "Text::Renderer::render(): capacity" << _capacity << "too small to render" << glyphCount << "glyphs", );

    /* Interleave the data into mapped buffer*/
    Containers::ArrayView<Vertex> vertices(static_cast<Vertex*>(bufferMapImplementation(_vertexBuffer,
        vertexCount*sizeof(Vertex))), vertexCount);
    CORRADE_INTERNAL_ASSERT_OUTPUT(vertices);
    std::copy(vertexData.begin(), vertexData.end(), vertices.begin());
    bufferUnmapImplementation(_vertexBuffer);

    /* Update index count */
    _mesh.setCount(indexCount);
}

#ifndef DOXYGEN_GENERATING_OUTPUT
template class MAGNUM_TEXT_EXPORT Renderer<2>;
template class MAGNUM_TEXT_EXPORT Renderer<3>;
#endif
#endif

}}
