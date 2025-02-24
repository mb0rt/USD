//
// Copyright 2016 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//
#include "pxr/imaging/garch/glApi.h"

// XXX We do not want to include specific HgiXX backends, but we need to do
// this temporarily until Storm has transitioned fully to Hgi.
#include "pxr/imaging/hgiGL/graphicsCmds.h"

#include "pxr/imaging/glf/diagnostic.h"

#include "pxr/imaging/hdSt/immediateDrawBatch.h"

#include "pxr/imaging/hdSt/bufferArrayRange.h"
#include "pxr/imaging/hdSt/commandBuffer.h"
#include "pxr/imaging/hdSt/drawItem.h"
#include "pxr/imaging/hdSt/drawItemInstance.h"
#include "pxr/imaging/hdSt/geometricShader.h"
#include "pxr/imaging/hdSt/glslProgram.h"
#include "pxr/imaging/hdSt/glConversions.h"
#include "pxr/imaging/hdSt/renderPassState.h"
#include "pxr/imaging/hdSt/resourceRegistry.h"
#include "pxr/imaging/hdSt/shaderCode.h"

#include "pxr/imaging/hd/debugCodes.h"
#include "pxr/imaging/hd/mesh.h"
#include "pxr/imaging/hd/perfLog.h"
#include "pxr/imaging/hd/tokens.h"

#include "pxr/base/tf/diagnostic.h"

PXR_NAMESPACE_OPEN_SCOPE


HdSt_ImmediateDrawBatch::HdSt_ImmediateDrawBatch(
    HdStDrawItemInstance * drawItemInstance)
    : HdSt_DrawBatch(drawItemInstance)
{
    _Init(drawItemInstance);
}

void
HdSt_ImmediateDrawBatch::_Init(HdStDrawItemInstance * drawItemInstance)
{
    HdSt_DrawBatch::_Init(drawItemInstance);
    drawItemInstance->SetBatchIndex(0);
    drawItemInstance->SetBatch(this);
    _bufferArraysHash =
        drawItemInstance->GetDrawItem()->GetBufferArraysHash();
}

HdSt_ImmediateDrawBatch::~HdSt_ImmediateDrawBatch()
{
}

HdSt_DrawBatch::ValidationResult
HdSt_ImmediateDrawBatch::Validate(bool deepValidation)
{
    if (!TF_VERIFY(!_drawItemInstances.empty())) {
        return ValidationResult::RebuildAllBatches;
    }

    HdStDrawItem const* batchItem = _drawItemInstances.front()->GetDrawItem();

    // check the hash to see if anything's been reallocated/migrated.
    // note that we just need to compare the hash of the first item,
    // since draw items are aggregated and ensure that they are sharing
    // the same buffer arrays.
    size_t bufferArraysHash = batchItem->GetBufferArraysHash();
    if (_bufferArraysHash != bufferArraysHash) {
        _bufferArraysHash = bufferArraysHash;
        return ValidationResult::RebuildBatch;
    }

    // Deep validation is flagged explicitly when a drawItem has changes to
    // its BARs (e.g. buffer spec, aggregation) or when its 
    // surface shader or geometric shader changes.
    if (deepValidation) {
        HD_TRACE_SCOPE("Immediate draw batch deep validation");

        // look through all draw items to be still compatible

        size_t numDrawItemInstances = _drawItemInstances.size();
        for (size_t item = 0; item < numDrawItemInstances; ++item) {
            HdStDrawItem const * drawItem
                = _drawItemInstances[item]->GetDrawItem();

            if (!TF_VERIFY(drawItem->GetGeometricShader())) {
                return ValidationResult::RebuildAllBatches;
            }

            if (!_IsAggregated(batchItem, drawItem)) {
                return ValidationResult::RebuildAllBatches;
            }
        }
    }

    return ValidationResult::ValidBatch;
}

void
HdSt_ImmediateDrawBatch::PrepareDraw(
    HdStRenderPassStateSharedPtr const &renderPassState,
    HdStResourceRegistrySharedPtr const &resourceRegistry)
{
}

static int
_GetElementOffset(HdBufferArrayRangeSharedPtr const& range)
{
    return range? range->GetElementOffset() : 0;
}

void
HdSt_ImmediateDrawBatch::ExecuteDraw(
    HgiGraphicsCmds *gfxCmds,
    HdStRenderPassStateSharedPtr const &renderPassState,
    HdStResourceRegistrySharedPtr const &resourceRegistry)
{
    HgiGLGraphicsCmds* glGfxCmds = dynamic_cast<HgiGLGraphicsCmds*>(gfxCmds);

    if (glGfxCmds) {
        // XXX Tmp code path to allow non-hgi code to insert functions into
        // HgiGL ops-stack. Will be removed once Storms uses Hgi everywhere
        auto executeDrawOp = [this, renderPassState, resourceRegistry] {
            this->_ExecuteDraw(renderPassState, resourceRegistry);
        };
        glGfxCmds->InsertFunctionOp(executeDrawOp);
    } else {
        _ExecuteDraw(renderPassState, resourceRegistry);
    }
}

void
HdSt_ImmediateDrawBatch::_ExecuteDraw(
    HdStRenderPassStateSharedPtr const &renderPassState,
    HdStResourceRegistrySharedPtr const &resourceRegistry)
{
    HD_TRACE_FUNCTION();
    GLF_GROUP_FUNCTION();

    HdStBufferArrayRangeSharedPtr indexBarCurrent;
    HdStBufferArrayRangeSharedPtr topVisBarCurrent;
    HdStBufferArrayRangeSharedPtr elementBarCurrent;
    HdStBufferArrayRangeSharedPtr vertexBarCurrent;
    HdStBufferArrayRangeSharedPtr varyingBarCurrent;
    HdStBufferArrayRangeSharedPtr constantBarCurrent;
    HdStBufferArrayRangeSharedPtr fvarBarCurrent;
    HdStBufferArrayRangeSharedPtr instanceIndexBarCurrent;
    HdStBufferArrayRangeSharedPtr shaderBarCurrent;
    std::vector<HdStBufferArrayRangeSharedPtr> instanceBarCurrents;

    if (_drawItemInstances.empty()) return;

    if (!glUseProgram) return;  // glew initialized

    // bind program
    _DrawingProgram & program = _GetDrawingProgram(renderPassState,
                                                   /*indirect=*/false,
                                                   resourceRegistry);

    HdStGLSLProgramSharedPtr const &glslProgram = program.GetGLSLProgram();
    if (!TF_VERIFY(glslProgram)) return;
    if (!TF_VERIFY(glslProgram->Validate())) return;

    renderPassState->Bind();

    const HdSt_ResourceBinder &binder = program.GetBinder();
    const HdStShaderCodeSharedPtrVector &shaders = program.GetComposedShaders();

    GLuint programId = glslProgram->GetProgram()->GetRawResource();
    TF_VERIFY(programId);

    glUseProgram(programId);

    for (HdStShaderCodeSharedPtr const & shader : shaders) {
        shader->BindResources(programId, binder);
    }

    // Set up geometric shader states
    // all batch item should have the same geometric shader.
    HdSt_GeometricShaderSharedPtr const &geometricShader =
                                                program.GetGeometricShader();
    geometricShader->BindResources(programId, binder);

    renderPassState->ApplyStateFromGeometricShader(binder, geometricShader);

    if (geometricShader->IsPrimTypePatches()) {
        glPatchParameteri(GL_PATCH_VERTICES,
                          geometricShader->GetPrimitiveIndexSize());
    }

    size_t numItemsDrawn = 0;
    for (HdStDrawItemInstance const * drawItemInstance : _drawItemInstances) {
        if(!drawItemInstance->IsVisible()) {
            continue;
        }

        HdStDrawItem const * drawItem = drawItemInstance->GetDrawItem();

        ++numItemsDrawn;

        //
        // index buffer data
        //
        HdBufferArrayRangeSharedPtr const & indexBar_ =
            drawItem->GetTopologyRange();

        HdStBufferArrayRangeSharedPtr indexBar =
            std::static_pointer_cast<HdStBufferArrayRange>(indexBar_);

        if (indexBar && (!indexBar->IsAggregatedWith(indexBarCurrent))) {
            binder.UnbindBufferArray(indexBarCurrent);
            binder.BindBufferArray(indexBar);
            indexBarCurrent = indexBar;
        }

        //
        // topology visibility buffer data
        //
        HdBufferArrayRangeSharedPtr const &
            topVisBar_ = drawItem->GetTopologyVisibilityRange();

        HdStBufferArrayRangeSharedPtr topVisBar =
            std::static_pointer_cast<HdStBufferArrayRange>(topVisBar_);

        if (topVisBar && (!topVisBar->IsAggregatedWith(topVisBarCurrent))) {
            binder.UnbindInterleavedBuffer(topVisBarCurrent,
                                           HdTokens->topologyVisibility);
            binder.BindInterleavedBuffer(topVisBar,
                                         HdTokens->topologyVisibility);
            topVisBarCurrent = topVisBar;
        }

        //
        // per-face buffer data (fetched through ElementID in primitiveParam)
        //
        HdBufferArrayRangeSharedPtr const & elementBar_ =
            drawItem->GetElementPrimvarRange();

        HdStBufferArrayRangeSharedPtr elementBar =
            std::static_pointer_cast<HdStBufferArrayRange>(elementBar_);

        if (elementBar && (!elementBar->IsAggregatedWith(elementBarCurrent))) {
            binder.UnbindBufferArray(elementBarCurrent);
            binder.BindBufferArray(elementBar);
            elementBarCurrent = elementBar;
        }

        //
        // vertex attrib buffer data
        //
        HdBufferArrayRangeSharedPtr const & vertexBar_ =
            drawItem->GetVertexPrimvarRange();

        HdStBufferArrayRangeSharedPtr vertexBar =
            std::static_pointer_cast<HdStBufferArrayRange>(vertexBar_);

        if (vertexBar && (!vertexBar->IsAggregatedWith(vertexBarCurrent))) {
            binder.UnbindBufferArray(vertexBarCurrent);
            binder.BindBufferArray(vertexBar);
            vertexBarCurrent = vertexBar;
        }

        //
        // varying buffer data 
        //
        HdBufferArrayRangeSharedPtr const & varyingBar_ =
            drawItem->GetVaryingPrimvarRange();

        HdStBufferArrayRangeSharedPtr varyingBar =
            std::static_pointer_cast<HdStBufferArrayRange>(varyingBar_);

        if (varyingBar && (!varyingBar->IsAggregatedWith(varyingBarCurrent))) {
            binder.UnbindBufferArray(varyingBarCurrent);
            binder.BindBufferArray(varyingBar);
            varyingBarCurrent = varyingBar;
        }

        //
        // constant (uniform) buffer data
        //
        HdBufferArrayRangeSharedPtr const & constantBar_ =
            drawItem->GetConstantPrimvarRange();

        HdStBufferArrayRangeSharedPtr constantBar =
            std::static_pointer_cast<HdStBufferArrayRange>(constantBar_);

        if (constantBar && (!constantBar->IsAggregatedWith(constantBarCurrent))) {
            binder.UnbindConstantBuffer(constantBarCurrent);
            binder.BindConstantBuffer(constantBar);
            constantBarCurrent = constantBar;
        }

        //
        // facevarying buffer data
        //
        HdBufferArrayRangeSharedPtr const & fvarBar_ =
            drawItem->GetFaceVaryingPrimvarRange();

        HdStBufferArrayRangeSharedPtr fvarBar =
            std::static_pointer_cast<HdStBufferArrayRange>(fvarBar_);

        if (fvarBar && (!fvarBar->IsAggregatedWith(fvarBarCurrent))) {
            binder.UnbindBufferArray(fvarBarCurrent);
            binder.BindBufferArray(fvarBar);
            fvarBarCurrent = fvarBar;
        }

        //
        // instance buffer data
        //
        int instancerNumLevels = drawItem->GetInstancePrimvarNumLevels();
        int instanceIndexWidth = instancerNumLevels + 1;
        for (int i = 0; i < instancerNumLevels; ++i) {
            HdBufferArrayRangeSharedPtr const & instanceBar_ =
                drawItem->GetInstancePrimvarRange(i);

            HdStBufferArrayRangeSharedPtr instanceBar =
                std::static_pointer_cast<HdStBufferArrayRange>(instanceBar_);

            if (instanceBar) {
                if (static_cast<size_t>(i) >= instanceBarCurrents.size()) {
                    instanceBarCurrents.push_back(instanceBar);
                    binder.BindInstanceBufferArray(instanceBar, i);
                    continue;
                } else if (!instanceBar->IsAggregatedWith(
                               instanceBarCurrents[i])) {
                    binder.UnbindInstanceBufferArray(instanceBarCurrents[i], i);
                    binder.BindInstanceBufferArray(instanceBar, i);
                }
                instanceBarCurrents[i] = instanceBar;
            }
        }

        //
        // instance index indirection buffer
        //
        HdBufferArrayRangeSharedPtr const & instanceIndexBar_ =
            drawItem->GetInstanceIndexRange();

        HdStBufferArrayRangeSharedPtr instanceIndexBar =
            std::static_pointer_cast<HdStBufferArrayRange>(instanceIndexBar_);

        if (instanceIndexBar && 
            (!instanceIndexBar->IsAggregatedWith(instanceIndexBarCurrent))) {
            binder.UnbindBufferArray(instanceIndexBarCurrent);
            binder.BindBufferArray(instanceIndexBar);
            instanceIndexBarCurrent = instanceIndexBar;
        }

        //
        // shader buffer
        //
        HdBufferArrayRangeSharedPtr const & shaderBar_ =
            !program.GetMaterialNetworkShader()
                ? HdStBufferArrayRangeSharedPtr()
                : program.GetMaterialNetworkShader()->GetShaderData();
        HdStBufferArrayRangeSharedPtr shaderBar =
            std::static_pointer_cast<HdStBufferArrayRange> (shaderBar_);

        // shaderBar isn't needed when the material is overridden
        if (shaderBar && (!shaderBar->IsAggregatedWith(shaderBarCurrent))) {
            if (shaderBarCurrent) {
                binder.UnbindBuffer(HdTokens->materialParams,
                                    shaderBarCurrent->GetResource());
            }
            binder.BindBuffer(HdTokens->materialParams,
                              shaderBar->GetResource());
            shaderBarCurrent = shaderBar;
        }

        //
        // shader textures
        //
        if (program.GetMaterialNetworkShader()) {
            program.GetMaterialNetworkShader()->BindResources(
                programId, binder);
        }

        /*
          Drawing coord is a unified cursor which locates a subset of
          aggregated buffer in GPU. The primary role of drawing coord is
          to provide a way to access buffers from glsl shader code.

          We have some aggregated buffers of different granularities.
          They are associated to class/variability specifiers in GL/prman spec.
          ( see http://renderman.pixar.com/view/Appnote22 )

          |   | drawing coord |  hd buffer   |     OpenGL     |     PRMan      |
          ----------------------------------------------------------------------
          | 0 | ModelDC       |  (reserved)  |    uniform     |    constant    |
          | 1 | ConstantDC    |  constantBar |    uniform     |    constant    |
          | 2 | VertexDC      |  vertexBar   |gl_BaseVertex(^)| vertex/varying |
          | 3 | ElementDC     |  elementBar  |       (*)      |    uniform     |
          | 4 | PrimitiveDC   |  indexBar    | gl_PrimitiveID |       (*)      |
          | 5 | FVarDC        |  fvarBar     | gl_PrimitiveID |    facevarying |
          | 6 | InstanceIndex |  inst-idxBar | (gl_InstanceID)|      n/a       |
          | 7 | ShaderDC      |  shaderBar   |    uniform     |                |
          | 8 | TopVisDC      |  topVisBar   |                |                |
          | 9 | VaryingDC     |  varyingBar  |gl_BaseVertex(^)|    varying     |
          | 10| InstanceDC[0] |  instanceBar | (gl_InstanceID)|    constant    |
          | 11| InstanceDC[1] |  instanceBar | (gl_InstanceID)|    constant    |
          |...| ...           |  instanceBar | (gl_InstanceID)|    constant    |
          ----------------------------------------------------------------------

          We put these offsets into 4 variables,
           - ivec4 drawingCoord0  {ModelDC, ConstantDC, ElementDC, PrimitiveDC}
           - ivec4 drawingCoord1  {FVarDC, InstanceIndex, ShaderDC, VertexDC}
           - ivec2 drawingCoord2  {TopoVisibilityDC, VaryingDC}
           - int[] drawingCoordI  (InstanceDC)
          so that the shaders can access any of these aggregated data.

          (^) gl_BaseVertex requires GLSL 4.60 or the ARB_shader_draw_parameters
              extension. We simply plumb the baseVertex(Offset) as a generic 
              solution.
          (*) primitiveParam buffer can be used to reinterpret GL-primitive
              ID back to element ID.
         */

        int vertexOffset = 0;
        int vertexCount = 0;
        if (vertexBar) {
            vertexOffset = vertexBar->GetElementOffset();
            vertexCount = vertexBar->GetNumElements();
        }

        //
        // Get parameters from our buffer range objects to
        // allow drawing to access the correct elements from
        // aggregated buffers.
        //
        int numIndicesPerPrimitive = geometricShader->GetPrimitiveIndexSize();
        int indexCount = indexBar ? indexBar->GetNumElements() * numIndicesPerPrimitive : 0;
        int firstIndex = indexBar ? indexBar->GetElementOffset() * numIndicesPerPrimitive : 0;
        int baseVertex = vertexOffset;
        int instanceCount = instanceIndexBar
            ? instanceIndexBar->GetNumElements()/instanceIndexWidth : 1;

        // if delegate fails to get vertex primvars, it could be empty.
        // skip the drawitem to prevent drawing uninitialized vertices.
        if (vertexCount == 0) continue;

        // update standalone uniforms
        int drawingCoord0[4] = {
            0, // reserved for modelBar
            _GetElementOffset(constantBar),
            _GetElementOffset(elementBar),
            _GetElementOffset(indexBar),
        };
        int drawingCoord1[4] = {
            _GetElementOffset(fvarBar),
            _GetElementOffset(instanceIndexBar),
            _GetElementOffset(shaderBar),
            baseVertex
        };
        int drawingCoord2[2] = {
            _GetElementOffset(topVisBar),
            _GetElementOffset(varyingBar),
        };
        binder.BindUniformi(HdTokens->drawingCoord0, 4, drawingCoord0);
        binder.BindUniformi(HdTokens->drawingCoord1, 4, drawingCoord1);
        binder.BindUniformi(HdTokens->drawingCoord2, 2, drawingCoord2);

        // instance coordinates
        std::vector<int> instanceDrawingCoords(instancerNumLevels);
        for (int i = 0; i < instancerNumLevels; ++i) {
            instanceDrawingCoords[i] =
                _GetElementOffset(instanceBarCurrents[i]);
        }
        if (instancerNumLevels > 0) {
            binder.BindUniformArrayi(HdTokens->drawingCoordI,
                                     instancerNumLevels, &instanceDrawingCoords[0]);
        }

        if (indexCount > 0 && indexBar) {
            glDrawElementsInstancedBaseVertex(
                HdStGLConversions::GetPrimitiveMode(geometricShader.get()),
                indexCount,
                GL_UNSIGNED_INT, // GL_INT is invalid: indexBar->GetResource(HdTokens->indices)->GetGLDataType(),
                (void *)(firstIndex * sizeof(uint32_t)),
                instanceCount,
                baseVertex);
        } else if (vertexCount > 0) {
            glDrawArraysInstanced(
                HdStGLConversions::GetPrimitiveMode(geometricShader.get()),
                baseVertex,
                vertexCount,
                instanceCount);
        }

        if (program.GetMaterialNetworkShader()) {
            program.GetMaterialNetworkShader()->UnbindResources(
                programId, binder);
        }

        HD_PERF_COUNTER_INCR(HdPerfTokens->drawCalls);
    }

    HD_PERF_COUNTER_ADD(HdTokens->itemsDrawn, numItemsDrawn);

    for (HdStShaderCodeSharedPtr const & shader : shaders) {
        shader->UnbindResources(programId, binder);
    }
    geometricShader->UnbindResources(programId, binder);

    // unbind (make non resident all bindless buffers)
    if (constantBarCurrent)
        binder.UnbindConstantBuffer(constantBarCurrent);
    if (vertexBarCurrent)
        binder.UnbindBufferArray(vertexBarCurrent);
    if (varyingBarCurrent)
        binder.UnbindBufferArray(varyingBarCurrent);
    if (elementBarCurrent)
        binder.UnbindBufferArray(elementBarCurrent);
    if (fvarBarCurrent)
        binder.UnbindBufferArray(fvarBarCurrent);
    for (size_t i = 0; i < instanceBarCurrents.size(); ++i) {
        binder.UnbindInstanceBufferArray(instanceBarCurrents[i], i);
    }
    if (instanceIndexBarCurrent)
        binder.UnbindBufferArray(instanceIndexBarCurrent);
    if (indexBarCurrent)
        binder.UnbindBufferArray(indexBarCurrent);
    if (topVisBarCurrent)
        binder.UnbindBufferArray(topVisBarCurrent);
    if (shaderBarCurrent) {
        binder.UnbindBuffer(HdTokens->materialParams,
                            shaderBarCurrent->GetResource());
    }

    glUseProgram(0);

    renderPassState->Unbind();
}

PXR_NAMESPACE_CLOSE_SCOPE

