//
// Copyright (c) 2014 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// RendererD3D.cpp: Implementation of the base D3D Renderer.

#include "libGLESv2/renderer/d3d/RendererD3D.h"

#include "libGLESv2/renderer/d3d/IndexDataManager.h"
#include "libGLESv2/Framebuffer.h"
#include "libGLESv2/FramebufferAttachment.h"
#include "libGLESv2/ResourceManager.h"
#include "libGLESv2/State.h"
#include "libGLESv2/VertexArray.h"
#include "common/utilities.h"

namespace rx
{

RendererD3D::RendererD3D(egl::Display *display)
    : mDisplay(display),
      mCurrentClientVersion(2)
{
}

RendererD3D::~RendererD3D()
{
    for (auto &incompleteTexture : mIncompleteTextures)
    {
        incompleteTexture.second.set(NULL);
    }
    mIncompleteTextures.clear();
}

// static
RendererD3D *RendererD3D::makeRendererD3D(Renderer *renderer)
{
    ASSERT(HAS_DYNAMIC_TYPE(RendererD3D*, renderer));
    return static_cast<RendererD3D*>(renderer);
}

gl::Error RendererD3D::drawElements(const gl::Data &data,
                                    GLenum mode, GLsizei count, GLenum type,
                                    const GLvoid *indices, GLsizei instances,
                                    const RangeUI &indexRange)
{
    ASSERT(data.state->getCurrentProgramId() != 0);

    gl::ProgramBinary *programBinary = data.state->getCurrentProgramBinary();
    programBinary->updateSamplerMapping();

    gl::Error error = generateSwizzles(data);
    if (error.isError())
    {
        return error;
    }

    if (!applyPrimitiveType(mode, count))
    {
        return gl::Error(GL_NO_ERROR);
    }

    error = applyRenderTarget(data, mode, false);
    if (error.isError())
    {
        return error;
    }

    error = applyState(data, mode);
    if (error.isError())
    {
        return error;
    }

    gl::VertexArray *vao = data.state->getVertexArray();
    rx::TranslatedIndexData indexInfo;
    indexInfo.indexRange = indexRange;
    error = applyIndexBuffer(indices, vao->getElementArrayBuffer(), count, mode, type, &indexInfo);
    if (error.isError())
    {
        return error;
    }

    GLsizei vertexCount = indexInfo.indexRange.length() + 1;
    error = applyVertexBuffer(*data.state, indexInfo.indexRange.start, vertexCount, instances);
    if (error.isError())
    {
        return error;
    }

    bool transformFeedbackActive = applyTransformFeedbackBuffers(data);
    // Transform feedback is not allowed for DrawElements, this error should have been caught at the API validation
    // layer.
    ASSERT(!transformFeedbackActive);

    error = applyShaders(data, transformFeedbackActive);
    if (error.isError())
    {
        return error;
    }

    error = applyTextures(data);
    if (error.isError())
    {
        return error;
    }

    error = applyUniformBuffers(data);
    if (error.isError())
    {
        return error;
    }

    if (!skipDraw(data, mode))
    {
        error = drawElements(mode, count, type, indices, vao->getElementArrayBuffer(), indexInfo, instances);
        if (error.isError())
        {
            return error;
        }
    }

    return gl::Error(GL_NO_ERROR);
}

gl::Error RendererD3D::drawArrays(const gl::Data &data,
                                  GLenum mode, GLint first,
                                  GLsizei count, GLsizei instances)
{
    ASSERT(data.state->getCurrentProgramId() != 0);

    gl::ProgramBinary *programBinary = data.state->getCurrentProgramBinary();
    programBinary->updateSamplerMapping();

    gl::Error error = generateSwizzles(data);
    if (error.isError())
    {
        return error;
    }

    if (!applyPrimitiveType(mode, count))
    {
        return gl::Error(GL_NO_ERROR);
    }

    error = applyRenderTarget(data, mode, false);
    if (error.isError())
    {
        return error;
    }

    error = applyState(data, mode);
    if (error.isError())
    {
        return error;
    }

    error = applyVertexBuffer(*data.state, first, count, instances);
    if (error.isError())
    {
        return error;
    }

    bool transformFeedbackActive = applyTransformFeedbackBuffers(data);

    error = applyShaders(data, transformFeedbackActive);
    if (error.isError())
    {
        return error;
    }

    error = applyTextures(data);
    if (error.isError())
    {
        return error;
    }

    error = applyUniformBuffers(data);
    if (error.isError())
    {
        return error;
    }

    if (!skipDraw(data, mode))
    {
        error = drawArrays(mode, count, instances, transformFeedbackActive);
        if (error.isError())
        {
            return error;
        }

        if (transformFeedbackActive)
        {
            markTransformFeedbackUsage(data);
        }
    }

    return gl::Error(GL_NO_ERROR);
}

gl::Error RendererD3D::generateSwizzles(const gl::Data &data, gl::SamplerType type)
{
    gl::ProgramBinary *programBinary = data.state->getCurrentProgramBinary();

    size_t samplerRange = programBinary->getUsedSamplerRange(type);

    for (size_t i = 0; i < samplerRange; i++)
    {
        GLenum textureType = programBinary->getSamplerTextureType(type, i);
        GLint textureUnit = programBinary->getSamplerMapping(type, i, *data.caps);
        if (textureUnit != -1)
        {
            gl::Texture *texture = data.state->getSamplerTexture(textureUnit, textureType);
            ASSERT(texture);
            if (texture->getSamplerState().swizzleRequired())
            {
                gl::Error error = generateSwizzle(texture);
                if (error.isError())
                {
                    return error;
                }
            }
        }
    }

    return gl::Error(GL_NO_ERROR);
}

gl::Error RendererD3D::generateSwizzles(const gl::Data &data)
{
    gl::Error error = generateSwizzles(data, gl::SAMPLER_VERTEX);
    if (error.isError())
    {
        return error;
    }

    error = generateSwizzles(data, gl::SAMPLER_PIXEL);
    if (error.isError())
    {
        return error;
    }

    return gl::Error(GL_NO_ERROR);
}

// Applies the render target surface, depth stencil surface, viewport rectangle and
// scissor rectangle to the renderer
gl::Error RendererD3D::applyRenderTarget(const gl::Data &data, GLenum drawMode, bool ignoreViewport)
{
    const gl::Framebuffer *framebufferObject = data.state->getDrawFramebuffer();
    ASSERT(framebufferObject && framebufferObject->completeness(data) == GL_FRAMEBUFFER_COMPLETE);

    gl::Error error = applyRenderTarget(framebufferObject);
    if (error.isError())
    {
        return error;
    }

    float nearZ, farZ;
    data.state->getDepthRange(&nearZ, &farZ);
    setViewport(data.state->getViewport(), nearZ, farZ, drawMode,
                data.state->getRasterizerState().frontFace, ignoreViewport);

    setScissorRectangle(data.state->getScissor(), data.state->isScissorTestEnabled());

    return gl::Error(GL_NO_ERROR);
}

// Applies the fixed-function state (culling, depth test, alpha blending, stenciling, etc) to the Direct3D device
gl::Error RendererD3D::applyState(const gl::Data &data, GLenum drawMode)
{
    const gl::Framebuffer *framebufferObject = data.state->getDrawFramebuffer();
    int samples = framebufferObject->getSamples(data);

    gl::RasterizerState rasterizer = data.state->getRasterizerState();
    rasterizer.pointDrawMode = (drawMode == GL_POINTS);
    rasterizer.multiSample = (samples != 0);

    gl::Error error = setRasterizerState(rasterizer);
    if (error.isError())
    {
        return error;
    }

    unsigned int mask = 0;
    if (data.state->isSampleCoverageEnabled())
    {
        GLclampf coverageValue;
        bool coverageInvert = false;
        data.state->getSampleCoverageParams(&coverageValue, &coverageInvert);
        if (coverageValue != 0)
        {
            float threshold = 0.5f;

            for (int i = 0; i < samples; ++i)
            {
                mask <<= 1;

                if ((i + 1) * coverageValue >= threshold)
                {
                    threshold += 1.0f;
                    mask |= 1;
                }
            }
        }

        if (coverageInvert)
        {
            mask = ~mask;
        }
    }
    else
    {
        mask = 0xFFFFFFFF;
    }
    error = setBlendState(framebufferObject, data.state->getBlendState(), data.state->getBlendColor(), mask);
    if (error.isError())
    {
        return error;
    }

    error = setDepthStencilState(data.state->getDepthStencilState(), data.state->getStencilRef(),
                                 data.state->getStencilBackRef(), rasterizer.frontFace == GL_CCW);
    if (error.isError())
    {
        return error;
    }

    return gl::Error(GL_NO_ERROR);
}

bool RendererD3D::applyTransformFeedbackBuffers(const gl::Data &data)
{
    gl::TransformFeedback *curTransformFeedback = data.state->getCurrentTransformFeedback();
    if (curTransformFeedback && curTransformFeedback->isStarted() && !curTransformFeedback->isPaused())
    {
        applyTransformFeedbackBuffers(*data.state);
        return true;
    }
    else
    {
        return false;
    }
}

// Applies the shaders and shader constants to the Direct3D device
gl::Error RendererD3D::applyShaders(const gl::Data &data, bool transformFeedbackActive)
{
    gl::ProgramBinary *programBinary = data.state->getCurrentProgramBinary();

    gl::VertexFormat inputLayout[gl::MAX_VERTEX_ATTRIBS];
    gl::VertexFormat::GetInputLayout(inputLayout, programBinary, *data.state);

    const gl::Framebuffer *fbo = data.state->getDrawFramebuffer();

    gl::Error error = applyShaders(programBinary, inputLayout, fbo, data.state->getRasterizerState().rasterizerDiscard, transformFeedbackActive);
    if (error.isError())
    {
        return error;
    }

    return programBinary->applyUniforms();
}

// For each Direct3D sampler of either the pixel or vertex stage,
// looks up the corresponding OpenGL texture image unit and texture type,
// and sets the texture and its addressing/filtering state (or NULL when inactive).
gl::Error RendererD3D::applyTextures(const gl::Data &data, gl::SamplerType shaderType,
                                     const FramebufferTextureSerialArray &framebufferSerials, size_t framebufferSerialCount)
{
    gl::ProgramBinary *programBinary = data.state->getCurrentProgramBinary();

    size_t samplerRange = programBinary->getUsedSamplerRange(shaderType);
    for (size_t samplerIndex = 0; samplerIndex < samplerRange; samplerIndex++)
    {
        GLenum textureType = programBinary->getSamplerTextureType(shaderType, samplerIndex);
        GLint textureUnit = programBinary->getSamplerMapping(shaderType, samplerIndex, *data.caps);
        if (textureUnit != -1)
        {
            gl::Texture *texture = data.state->getSamplerTexture(textureUnit, textureType);
            ASSERT(texture);
            gl::SamplerState sampler = texture->getSamplerState();

            gl::Sampler *samplerObject = data.state->getSampler(textureUnit);
            if (samplerObject)
            {
                samplerObject->getState(&sampler);
            }

            // TODO: std::binary_search may become unavailable using older versions of GCC
            if (texture->isSamplerComplete(sampler, *data.textureCaps, *data.extensions, data.clientVersion) &&
                !std::binary_search(framebufferSerials.begin(), framebufferSerials.begin() + framebufferSerialCount, texture->getTextureSerial()))
            {
                gl::Error error = setSamplerState(shaderType, samplerIndex, texture, sampler);
                if (error.isError())
                {
                    return error;
                }

                error = setTexture(shaderType, samplerIndex, texture);
                if (error.isError())
                {
                    return error;
                }
            }
            else
            {
                // Texture is not sampler complete or it is in use by the framebuffer.  Bind the incomplete texture.
                gl::Texture *incompleteTexture = getIncompleteTexture(textureType);
                gl::Error error = setTexture(shaderType, samplerIndex, incompleteTexture);
                if (error.isError())
                {
                    return error;
                }
            }
        }
        else
        {
            // No texture bound to this slot even though it is used by the shader, bind a NULL texture
            gl::Error error = setTexture(shaderType, samplerIndex, NULL);
            if (error.isError())
            {
                return error;
            }
        }
    }

    // Set all the remaining textures to NULL
    size_t samplerCount = (shaderType == gl::SAMPLER_PIXEL) ? data.caps->maxTextureImageUnits
                                                            : data.caps->maxVertexTextureImageUnits;
    for (size_t samplerIndex = samplerRange; samplerIndex < samplerCount; samplerIndex++)
    {
        gl::Error error = setTexture(shaderType, samplerIndex, NULL);
        if (error.isError())
        {
            return error;
        }
    }

    return gl::Error(GL_NO_ERROR);
}

gl::Error RendererD3D::applyTextures(const gl::Data &data)
{
    FramebufferTextureSerialArray framebufferSerials;
    size_t framebufferSerialCount = getBoundFramebufferTextureSerials(data, &framebufferSerials);

    gl::Error error = applyTextures(data, gl::SAMPLER_VERTEX, framebufferSerials, framebufferSerialCount);
    if (error.isError())
    {
        return error;
    }

    error = applyTextures(data, gl::SAMPLER_PIXEL, framebufferSerials, framebufferSerialCount);
    if (error.isError())
    {
        return error;
    }

    return gl::Error(GL_NO_ERROR);
}

gl::Error RendererD3D::applyUniformBuffers(const gl::Data &data)
{
    gl::Program *programObject = data.resourceManager->getProgram(data.state->getCurrentProgramId());
    gl::ProgramBinary *programBinary = programObject->getProgramBinary();

    std::vector<gl::Buffer*> boundBuffers;

    for (unsigned int uniformBlockIndex = 0; uniformBlockIndex < programBinary->getActiveUniformBlockCount(); uniformBlockIndex++)
    {
        GLuint blockBinding = programObject->getUniformBlockBinding(uniformBlockIndex);

        if (data.state->getIndexedUniformBuffer(blockBinding)->id() == 0)
        {
            // undefined behaviour
            return gl::Error(GL_INVALID_OPERATION, "It is undefined behaviour to have a used but unbound uniform buffer.");
        }
        else
        {
            gl::Buffer *uniformBuffer = data.state->getIndexedUniformBuffer(blockBinding);
            ASSERT(uniformBuffer);
            boundBuffers.push_back(uniformBuffer);
        }
    }

    return programBinary->applyUniformBuffers(boundBuffers, *data.caps);
}

bool RendererD3D::skipDraw(const gl::Data &data, GLenum drawMode)
{
    if (drawMode == GL_POINTS)
    {
        // ProgramBinary assumes non-point rendering if gl_PointSize isn't written,
        // which affects varying interpolation. Since the value of gl_PointSize is
        // undefined when not written, just skip drawing to avoid unexpected results.
        if (!data.state->getCurrentProgramBinary()->usesPointSize())
        {
            // This is stictly speaking not an error, but developers should be
            // notified of risking undefined behavior.
            ERR("Point rendering without writing to gl_PointSize.");

            return true;
        }
    }
    else if (gl::IsTriangleMode(drawMode))
    {
        if (data.state->getRasterizerState().cullFace && data.state->getRasterizerState().cullMode == GL_FRONT_AND_BACK)
        {
            return true;
        }
    }

    return false;
}

void RendererD3D::markTransformFeedbackUsage(const gl::Data &data)
{
    for (size_t i = 0; i < data.caps->maxTransformFeedbackSeparateAttributes; i++)
    {
        gl::Buffer *buffer = data.state->getIndexedTransformFeedbackBuffer(i);
        if (buffer)
        {
            buffer->markTransformFeedbackUsage();
        }
    }
}

size_t RendererD3D::getBoundFramebufferTextureSerials(const gl::Data &data,
                                                      FramebufferTextureSerialArray *outSerialArray)
{
    size_t serialCount = 0;

    const gl::Framebuffer *drawFramebuffer = data.state->getDrawFramebuffer();
    for (unsigned int i = 0; i < gl::IMPLEMENTATION_MAX_DRAW_BUFFERS; i++)
    {
        gl::FramebufferAttachment *attachment = drawFramebuffer->getColorbuffer(i);
        if (attachment && attachment->isTexture())
        {
            gl::Texture *texture = attachment->getTexture();
            (*outSerialArray)[serialCount++] = texture->getTextureSerial();
        }
    }

    gl::FramebufferAttachment *depthStencilAttachment = drawFramebuffer->getDepthOrStencilbuffer();
    if (depthStencilAttachment && depthStencilAttachment->isTexture())
    {
        gl::Texture *depthStencilTexture = depthStencilAttachment->getTexture();
        (*outSerialArray)[serialCount++] = depthStencilTexture->getTextureSerial();
    }

    std::sort(outSerialArray->begin(), outSerialArray->begin() + serialCount);

    return serialCount;
}

gl::Texture *RendererD3D::getIncompleteTexture(GLenum type)
{
    if (mIncompleteTextures.find(type) == mIncompleteTextures.end())
    {
        const GLubyte color[] = { 0, 0, 0, 255 };
        const gl::PixelUnpackState incompleteUnpackState(1);

        gl::Texture* t = NULL;
        switch (type)
        {
          default:
            UNREACHABLE();
            // default falls through to TEXTURE_2D

          case GL_TEXTURE_2D:
            {
                gl::Texture2D *incomplete2d = new gl::Texture2D(createTexture(GL_TEXTURE_2D), gl::Texture::INCOMPLETE_TEXTURE_ID);
                incomplete2d->setImage(0, 1, 1, GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, incompleteUnpackState, color);
                t = incomplete2d;
            }
            break;

          case GL_TEXTURE_CUBE_MAP:
            {
              gl::TextureCubeMap *incompleteCube = new gl::TextureCubeMap(createTexture(GL_TEXTURE_CUBE_MAP), gl::Texture::INCOMPLETE_TEXTURE_ID);

              incompleteCube->setImage(GL_TEXTURE_CUBE_MAP_POSITIVE_X, 0, 1, 1, GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, incompleteUnpackState, color);
              incompleteCube->setImage(GL_TEXTURE_CUBE_MAP_NEGATIVE_X, 0, 1, 1, GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, incompleteUnpackState, color);
              incompleteCube->setImage(GL_TEXTURE_CUBE_MAP_POSITIVE_Y, 0, 1, 1, GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, incompleteUnpackState, color);
              incompleteCube->setImage(GL_TEXTURE_CUBE_MAP_NEGATIVE_Y, 0, 1, 1, GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, incompleteUnpackState, color);
              incompleteCube->setImage(GL_TEXTURE_CUBE_MAP_POSITIVE_Z, 0, 1, 1, GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, incompleteUnpackState, color);
              incompleteCube->setImage(GL_TEXTURE_CUBE_MAP_NEGATIVE_Z, 0, 1, 1, GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, incompleteUnpackState, color);

              t = incompleteCube;
            }
            break;

          case GL_TEXTURE_3D:
            {
                gl::Texture3D *incomplete3d = new gl::Texture3D(createTexture(GL_TEXTURE_3D), gl::Texture::INCOMPLETE_TEXTURE_ID);
                incomplete3d->setImage(0, 1, 1, 1, GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, incompleteUnpackState, color);

                t = incomplete3d;
            }
            break;

          case GL_TEXTURE_2D_ARRAY:
            {
                gl::Texture2DArray *incomplete2darray = new gl::Texture2DArray(createTexture(GL_TEXTURE_2D_ARRAY), gl::Texture::INCOMPLETE_TEXTURE_ID);
                incomplete2darray->setImage(0, 1, 1, 1, GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, incompleteUnpackState, color);

                t = incomplete2darray;
            }
            break;
        }

        mIncompleteTextures[type].set(t);
    }

    return mIncompleteTextures[type].get();
}

}