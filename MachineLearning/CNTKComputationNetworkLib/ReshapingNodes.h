// ReshapingNodes.h -- collection of nodes that reshape or sub-sample matrices leading to layout changes
//
// <copyright file="NonlinearityNodes.h" company="Microsoft">
//     Copyright (c) Microsoft Corporation.  All rights reserved.
// </copyright>
//
#pragma once

#include <unordered_set>
#include <map>
#include <string>
#include <vector>
#include <stdexcept>
#include <list>
#include <memory>
#include <algorithm>
#include <assert.h>
#include <atomic>
#include <sstream>
#include <iostream>

#include "Basics.h"
#include "Matrix.h"
#include "ComputationNode.h"

namespace Microsoft { namespace MSR { namespace CNTK {

    // -----------------------------------------------------------------------
    // ReshapingNodeBase (input) -- base class for nodes that reshape
    // -----------------------------------------------------------------------

    template<class ElemType>
    class ReshapingNodeBase : public ComputationNode<ElemType>, public NumInputs<1>
    {
        typedef ComputationNode<ElemType> Base; UsingComputationNodeMembers;
    public:
        ReshapingNodeBase(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name)
        { }

        // stack K consecutive frames into a single frame that is K times taller
        static void Stack(const FrameRange & frameRange, /*const*/ Base * from, Base * to, size_t K, bool addTo)
        {
            // example
            //  input: T=2, D=2, K=3, S=2 (abcdef and uvwxyz)
            //   abc def
            //   ABC DEF
            //  
            //   uvw xyz
            //   UVW XYZ
            //  target:
            //   a d
            //   A D
            //   b e
            //   B E
            //   c f
            //   C F
            //  
            //   u x
            //   U X
            //   v y
            //   V Y
            //   w z
            //   W Z
            // underlying matrix storage is actually this:
            //  input:
            //   aubvcw dxeyfz
            //   AUBVCW DXEYFZ
            //  target:
            //   abcuvw defxyz
            //   ABCUVW DEFXYZ

            // I.e. this operation swaps index dimensions of a tensor:
            //   The input is a tensor of the form (D,       S, M, K, T).
            //   The output is of the form         (D, K, M, S,       T).
            //     K = stacking factor
            //     T = target steps
            //     S = #sequences
            //     D = featDim
            //     M = 1, thrown in for generality of underlying Matrix function

            // We operate on the target layout, frameRange refers to result, not the input.
            // The input layout is different, but reshaping the input to output dimensions will allow us to pull out the right values anyway.
            auto from0 = from->FunctionValues().Reshaped(to->GetNumRows(), to->GetNumCols());
            auto fromSlice0 = from->DataSlice(from0, frameRange);   // we operate on target layout
            auto toSlice0 = to->ValueSlice(frameRange);
            // now we got views on the right ranges of values, but with weird dimensions

            // reshape them into a unified view with D being the row dimension, and (S,M,K,T) the column dimension
            size_t D = from->GetNumRows();
            size_t SMKT = from->GetNumCols();
            auto fromSlice = fromSlice0.Reshaped(D, SMKT);
            auto toSlice = toSlice0.Reshaped(D, SMKT);

            // now to the shuffle dance
            size_t S = to->GetNumParallelSequences();
            size_t T = to->GetNumTimeSteps();
            size_t M = 1;
            Matrix<ElemType>::TensorShuffleScaleAndAdd(addTo ? 1.0f : 0, fromSlice, D, S, M, K, T, 1.0f, toSlice, toSlice);
        }

        static void Unstack(const FrameRange & frameRange, const Base * from, Base * to, size_t K, bool addTo)
        {
            frameRange; from; to; K; addTo;
            NOT_IMPLEMENTED
        }
    };

#define UsingReshapingNodeBaseMembers UsingComputationNodeMembersBoilerplate

    // -----------------------------------------------------------------------
    // ReshapeNode (input) -- reshape input matrix
    //
    // If input has no layout, then this reshapes the input matrix
    // from (rows x cols) to (newRows x (cols / newRows * rows)).
    //
    // If input has a layout, then it changes the number of time steps, i.e.
    // from (rows x T time steps) to (newRows x (T / newRows * rows) time steps).
    // E.g. going from rows=20 to newRows=40 groups two consecutive time steps into one.
    // In this case, multiple parallel sequences are treated independently.
    // -----------------------------------------------------------------------

    template<class ElemType>
    class ReshapeNode : public ReshapingNodeBase<ElemType>
    {
        typedef ReshapingNodeBase<ElemType> Base; UsingReshapingNodeBaseMembers;
        static const std::wstring TypeName() { return L"Reshape"; }
    public:
        ReshapeNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name),
            m_numRows(0),
            m_imageLayout(0, 0, 0)
        { }
        ReshapeNode(DEVICEID_TYPE deviceId, const wstring & name, size_t numRows, const ImageLayout & imageLayout) :
            Base(deviceId, name),
            m_numRows(numRows),
            m_imageLayout(imageLayout)
        { }

        virtual void CopyTo(const ComputationNodePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const override
        {
            Base::CopyTo(nodeP, newName, flags);
            if (flags & CopyNodeFlags::copyNodeValue)
            {
                auto node = dynamic_pointer_cast<ReshapeNode<ElemType>>(nodeP); // TODO: change to Base for all
                node->m_numRows = m_numRows;
                node->m_imageLayout = m_imageLayout;
            }
        }

        virtual void SaveToFile(File& fstream) const override
        {
            Base::SaveToFile(fstream);
            fstream << m_numRows << m_imageLayout.width << m_imageLayout.height << m_imageLayout.channels;
        }

        virtual void LoadFromFile(File& fstream, size_t modelVersion) override
        {
            Base::LoadFromFile(fstream, modelVersion);
            fstream >> m_numRows >> m_imageLayout.width >> m_imageLayout.height >> m_imageLayout.channels;
        }

        virtual void InferImageDimsFromInputs()
        {
            InferImageDimsFromInput(0, true);
            InferImageDimensions();

            if (m_imageLayout.width == 0 || m_imageLayout.height == 0 || m_imageLayout.channels == 0)
            {
                m_outputImageLayout = ImageLayout(1, 1, m_numRows);
                if (m_inputImageLayout.width * m_inputImageLayout.channels != 1)
                    fprintf(stderr, "WARNING: Reshape operation cannot inherit image size information from its child. Image size info is lost.\n");
            }
            else
            {
                m_outputImageLayout = m_imageLayout;
            }
        }

        virtual void PrintSelfBeforeValidation(bool allowNulls = false) const
        {
            fprintf(stderr, "\nValidating --> %ls = %ls", NodeName().c_str(), OperationName().c_str());

            if (!IsLeaf())
            {
                fprintf(stderr, "(");
                for (size_t i = 0; i < ChildrenSize(); i++)
                {
                    ComputationNodePtr child = Inputs(i);
                    if (i > 0)
                        fprintf(stderr, ", ");

                    if (child == nullptr)
                    {
                        if (allowNulls)
                        {
                            fprintf(stderr, "NULL");
                            continue;
                        }
                        RuntimeError("One of the children is missing.");
                    }

                    fprintf(stderr, "%ls[%lu, %lu]", child->NodeName().c_str(), child->GetNumRows(), child->GetNumCols());
                }

                fprintf(stderr, ", NumOfRows=%lu, imageWidth=%lu, imageHeight=%lu, imageChannels=%lu)", m_numRows, m_imageLayout.width, m_imageLayout.height, m_imageLayout.channels);
            }
        }

        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            Base::Validate(isFinalValidationPass);

            size_t rows = Inputs(0)->GetNumRows(), cols = Inputs(0)->GetNumCols();
            // Note: During initial validation, cols may not be a multiple. E.g. cols may be 1 or 3. So we cannot check here whether the integer-multiple conditions are fulfilled.
            size_t newCols = cols * rows / m_numRows;
            if (isFinalValidationPass)
            {
                if ((m_numRows > rows && m_numRows % rows != 0) ||  // grouping columns
                    (m_numRows < rows && rows % m_numRows != 0))    // splitting columns
                    InvalidArgument("%ls %ls operation: output row dimension %d is not an integer multiple or divisor of input dimension %d", (int)m_numRows, (int)rows);
                if (!m_pMBLayout && rows * cols != m_numRows != newCols)    // sadly, cannot verify here if we have a layout, since current #cols may be bogus
                    LogicError("%ls %ls operation: unexpected dimension mismatch");
            }

            Resize(m_numRows, newCols);
            if (Inputs(0)->HasMBLayout())
            {
                if (!m_pMBLayout)
                    m_pMBLayout = make_shared<MBLayout>();  // mini-batch data: this generates its own layout
            }
            else
                assert(!m_pMBLayout);                       // reshaping non-mini-batch data
            InferImageDimsFromInputs();
        }

        virtual size_t UpdateFunctionMBSize(size_t numCols) override
        {
            // BUGBUG: numCols parameter is legacy and not really supported
            size_t rows = Inputs(0)->GetNumRows(), cols = Inputs(0)->GetNumCols();
            size_t newCols = cols * rows / m_numRows;
            if (!m_pMBLayout)               // if no layout, this node contains parameters independent of MB size, don't resize
                VerifySize(m_numRows, newCols);
            else
                Resize(m_numRows, newCols);
            return numCols;
        }

        // TODO: there seems to be semantic overlap between OnEvaluateBeginIteration() and UpdateFunctionMBSize()
        virtual void /*IComputationNode::*/OnEvaluateBeginIteration() override
        {
            if (m_pMBLayout)
            {
                // create the derived layout
                // BUGBUG: This assumes that the layout is complete at this point in time. RecurrentNodeBase makes the same assumption. Becomes invalid once we go sequence-to-sequence.
                m_pMBLayout->Init(GetNumParallelSequences(), Inputs(0)->GetNumTimeSteps() * Inputs(0)->GetNumRows() / m_numRows);
            }
        }

        // notes:
        //  - input and output have different time base
        //  - frameRange refers to *functionValues*
        virtual void /*ComputationNode::*/EvaluateThisNode(const FrameRange & frameRange) override
        {
            if (isNoop())       // no change in dimension: FunctionValues() will return our input directly
                return;

            size_t rows = Inputs(0)->GetNumRows(), cols = Inputs(0)->GetNumCols();
            size_t newCols = cols * rows / m_numRows;
            assert(newCols * m_numRows == cols * rows); // follows from above check
            VerifySize(m_numRows, newCols);

            // no layout case: this is indeed just a reshape
            // TODO: we could set up our functionValues to be a view into the input, to avoid the actual copy
            if (!m_pMBLayout)
                FunctionValues().Reshaped(newCols * m_numRows, 1) = Inputs(0)->FunctionValues().Reshaped(cols * rows, 1);   // copy the values as one long vector
            // layout case: reshape semantics happens across parallel seqeunces, i.e. requiring data shuffling
            else if (weStack())
                Base::Stack(frameRange, Inputs(0).get(), this, factor(), false/*addTo*/);
            else
                Base::Unstack(frameRange, Inputs(0).get(), this, factor(), false/*addTo*/);

#if 0
            // for example, going from rows=1 to newRows=2 with 2 parallel sequences, we go from
            //  t0s0 t0s1 t1s0 t1s1 t2s0 t2s1...
            // to
            //  t0s0 t1s0 t0s1 t1s1 t2s0 t3s0...
            // In presence of multiple sequences, this can actually not be done implemented as a matrix reshape operation.

            size_t rows = Inputs(0)->GetNumRows(), cols = Inputs(0)->GetNumCols();
            size_t newCols = cols * rows / m_numRows;
            assert(newCols * m_numRows == cols * rows); // follows from above check
            VerifySize(m_numRows, newCols);

#if 0       // TODO: finish this later, as it is merely an optimization of the below
            // simple case: it's an actual reshape operation
            if (!m_pMBLayout || (frameRange.IsAllFrames() && GetNumParallelSequences() == 1))
            {
                // interpret both as long vectors, then assign them over
                auto inputValuesAsVector = Inputs(0)->FunctionValues().Reshaped(cols * rows, 1);
                auto functionValuesAsVector = FunctionValues().Reshaped(newCols * m_newRows, 1);
                functionValuesAsVector = inputValuesAsVector;
                // BUGBUG: layout
                return;
            }
#endif

            // all frames but with multiple sequences -> take the easy way out--do it frame by frame
            // TODO: We will need a dimension-swap operation in class Matrix. That's where all this code should go.
            if (frameRange.IsAllFrames())
            {
                FrameRangeIteration range(m_pMBLayout, +1);
                for (auto t = range.begin(); t != range.end(); t++)
                    EvaluateThisNode(t);    // call ourselves with a sub-range
                return;
            }

            // process time step by time step
            // TODO: This has a lot to do with row slices.
            assert(m_pMBLayout);
            auto r = frameRange.GetSequenceRange(m_pMBLayout);          // TODO: use range-based loop; currently for (auto s:r) gives a compiler error
            for (auto s = r.begin(); s != r.end(); s++)                 // loop over all sequences
            //for (auto s : frameRange.GetSequenceRange(m_pMBLayout))   // compiler error
            //for (auto s : r)                                          // compiler error
            {
                if (weStack())                  // grouping  --we place a partial vector
                {
                    size_t tOut = frameRange.t() / factor();
                    size_t subVec = frameRange.t() % factor();
                    ValueSlice(FrameRange(tOut).Sequence(s)).AssignToRowSliceValuesOf(Inputs(0)->ValueSlice(frameRange.Sequence(s)), subVec * rows, rows);
                    // update layout flags
                    if (subVec != 0 && Inputs(0)->GetMBLayout()->Is(s, frameRange.t(), MinibatchPackingFlags::SequenceStart))
                        InvalidArgument("%ls %ls operation: found sentence start inside (not at start) of group being decimated", NodeName().c_str(), OperationName().c_str());
                    if (subVec != factor()-1 && Inputs(0)->GetMBLayout()->Is(s, frameRange.t(), MinibatchPackingFlags::SequenceEnd))
                        InvalidArgument("%ls %ls operation: found sentence end inside (not at end) of group being decimated", NodeName().c_str(), OperationName().c_str());
                    m_pMBLayout->Set(s, tOut, Inputs(0)->GetMBLayout()->Get(s, frameRange.t()));       // BUGBUG: only first/last one may have start/end flag
                }
                else                            // splitting  --we place multiple target vectors
                {
                    size_t tOut0 = frameRange.t() * factor();
                    for (size_t subVec = 0; subVec < factor(); subVec++)
                    {
                        size_t tOut = tOut0 + subVec;
                        ValueSlice(FrameRange(tOut).Sequence(s)).AssignRowSliceValuesOf(Inputs(0)->ValueSlice(frameRange.Sequence(s)), subVec * m_numRows, m_numRows);
                    }
                    // update layout flags
                    if (Inputs(0)->GetMBLayout()->Is(s, frameRange.t(), MinibatchPackingFlags::SequenceStart))
                        m_pMBLayout->Set(s, tOut0, MinibatchPackingFlags::SequenceStart);
                    if (Inputs(0)->GetMBLayout()->Is(s, frameRange.t(), MinibatchPackingFlags::SequenceEnd))
                        m_pMBLayout->Set(s, tOut0+factor()-1, MinibatchPackingFlags::SequenceStart);
                }
                // TODO: need to check consistency of gaps
            }
#endif
        }

        virtual void /*ComputationNode::*/ComputeInputPartial(const size_t inputIndex, const FrameRange & frameRange) override
        {
#if 1
            inputIndex; frameRange;
            LogicError("ReshapeNode::ComputeInputPartial: to be completed");
#else
            size_t rows = Inputs(0)->GradientValues().GetNumRows();

            size_t outputSamplesInRecurrentStep = GetNumParallelSequences() * rows / m_numRows;

            Matrix<ElemType> inputGradientValues = Inputs(0)->GradientSlice(frameRange/*TODO: delete this:*/.Check_t(GetNumParallelSequences(), m_pMBLayout));
            // BUGBUG: the following will fail since outputSamplesInRecurrentStep will not match m_pMBLayout. Need to find out what this means (currently layout is constant throughout the graph), and implement it correctly.
            Matrix<ElemType> gradientValues = GradientSlice(frameRange/*TODO: delete this:*/.Check(frameRange.t() * outputSamplesInRecurrentStep, outputSamplesInRecurrentStep, m_pMBLayout));

            size_t numRows = inputGradientValues.GetNumRows();
            inputGradientValues.Reshape(gradientValues.GetNumRows(), gradientValues.GetNumCols());
            inputGradientValues += gradientValues;
            inputGradientValues.Reshape(numRows, inputGradientValues.GetNumElements() / numRows);
#endif
        }

        // BUGBUG: there is a const and a non-const version of this function
        virtual const Matrix<ElemType>& FunctionValues() const
        {
            if (!isNoop())
                return *m_functionValues;
            else
                return Inputs(0)->FunctionValues();
        }

    private:
        size_t m_numRows;
        bool weStack() const { return m_numRows > Inputs(0)->GetNumRows(); }        // do we stack (multiple frames into one)
        size_t factor() const { return m_numRows > Inputs(0)->GetNumRows() ? m_numRows / Inputs(0)->GetNumRows() : Inputs(0)->GetNumRows() / m_numRows; }   // factor by which we stack or unstack
        bool isNoop() const { return m_numRows == Inputs(0)->GetNumRows(); }
        ImageLayout m_imageLayout;

        void InferImageDimensions()
        {
            if (m_imageLayout.width > 0)
            {
                if (m_imageLayout.height > 0)
                {
                    if (m_imageLayout.channels > 0)
                    {
                        if (m_imageLayout.GetNumElements() != m_numRows)
                            RuntimeError("Image dimensions do not match row size.");
                    }
                    else
                    {
                        if (m_numRows % (m_imageLayout.width * m_imageLayout.height) > 0)
                            RuntimeError("Image row size is not a multiple of specified image dimensions.");
                        else
                            m_imageLayout.channels = m_numRows / (m_imageLayout.width * m_imageLayout.height);
                    }
                }
                else
                {
                    if (m_imageLayout.channels > 0)
                    {
                        if (m_numRows % (m_imageLayout.width * m_imageLayout.channels) > 0)
                            RuntimeError("Image row size is not a multiple of specified image dimensions.");
                        else
                            m_imageLayout.height = m_numRows / (m_imageLayout.width * m_imageLayout.channels);
                    }
                    else
                    {
                        RuntimeError("At least two image dimensions must be specified.");
                    }
                }
            }
            else
            {
                if (m_imageLayout.height > 0)
                {
                    if (m_imageLayout.channels > 0)
                    {
                        if (m_numRows % (m_imageLayout.height * m_imageLayout.channels) > 0)
                            RuntimeError("Image row size is not a multiple of specified image dimensions.");
                        else
                            m_imageLayout.width = m_numRows / (m_imageLayout.height * m_imageLayout.channels);
                    }
                    else
                        RuntimeError("At least two image dimensions must be specified.");
                }
                else if (m_imageLayout.channels > 0)
                    RuntimeError("At least two image dimensions must be specified.");
            }
        }
    };

    template class ReshapeNode<float>;
    template class ReshapeNode<double>;

    // -----------------------------------------------------------------------
    // RowSliceNode (input)
    // this node extracts part of the input by rows as the output
    // it has to be continuous segments of rows since each column is treated as one sample
    // -----------------------------------------------------------------------

    template<class ElemType>
    class RowSliceNode : public ComputationNode<ElemType>, public NumInputs<1>
    {
        typedef ComputationNode<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"RowSlice"; }
    public:
        RowSliceNode(DEVICEID_TYPE deviceId, const wstring & name, size_t startIndex = 0, size_t numRows = 0) :
            Base(deviceId, name),
            m_startIndex(startIndex),
            m_numRows(numRows)
        { }

        virtual void CopyTo(const ComputationNodePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const override
        {
            Base::CopyTo(nodeP, newName, flags);
            auto node = dynamic_pointer_cast<RowSliceNode<ElemType>>(nodeP);

            node->m_startIndex = m_startIndex;
            node->m_numRows = m_numRows;
        }

        virtual void SaveToFile(File& fstream) const override
        {
            Base::SaveToFile(fstream);
            fstream << m_startIndex << m_numRows;
        }
        
        virtual void LoadFromFile(File& fstream, size_t modelVersion) override
        {
            Base::LoadFromFile(fstream, modelVersion);
            fstream >> m_startIndex >> m_numRows;
        }

        virtual void /*ComputationNode::*/ComputeInputPartial(const size_t /*inputIndex*/, const FrameRange & frameRange) override
        {
            Inputs(0)->GradientSlice(frameRange).AddToRowSliceValuesOf(GradientSlice(frameRange), m_startIndex, m_numRows);
        }

        virtual void /*ComputationNode::*/EvaluateThisNode(const FrameRange & frameRange) override
        {
            ValueSlice(frameRange).AssignRowSliceValuesOf(Inputs(0)->ValueSlice(frameRange), m_startIndex, m_numRows);
        }

        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            Base::Validate(isFinalValidationPass);

            if (isFinalValidationPass && Inputs(0)->GetNumRows() < m_startIndex + m_numRows)
                RuntimeError("RowSlice operation: m_startIndex + m_numRows exceeds number of rows in the input.");

            Resize(m_numRows, Inputs(0)->GetNumCols());
            InferMBLayoutFromInputsForStandardCase();
            InferImageDimsFromInputs(); 
        }

        virtual void InferImageDimsFromInputs()
        {
            InferImageDimsFromInput(0, true);
            m_outputImageLayout.height = m_numRows;        

            //WARNING: this node will destroy the image size information from the child
            if (m_inputImageLayout.width * m_inputImageLayout.channels != 1)
                fprintf(stderr, "WARNING: RowSlice operation cannot inherit image size information from its child. Image size info is lost.\n");
        }

    private:
        size_t m_startIndex, m_numRows;
    };

    template class RowSliceNode<float>; 
    template class RowSliceNode<double>;

    // -----------------------------------------------------------------------
    // RowStackNode (input0, input1, ...)
    // stacks multiple inputs on top of each other
    // -----------------------------------------------------------------------

    template<class ElemType>
    class RowStackNode : public ComputationNode<ElemType>   // note: not deriving from NumInputs<> like most other nodes, because this one takes a variable number of inputs
    {
        typedef ComputationNode<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"RowStack"; }
    public:
        RowStackNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name)
        { }

        virtual void CopyTo(const ComputationNodePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const override
        {
            Base::CopyTo(nodeP, newName, flags);
            if (flags & CopyNodeFlags::copyNodeChildren)
            {
                auto node = dynamic_pointer_cast<RowStackNode<ElemType>>(nodeP);
                node->m_startRowIndices = m_startRowIndices;
            }
        }

        virtual void /*ComputationNode::*/ComputeInputPartial(const size_t inputIndex, const FrameRange & frameRange) override
        {
            Inputs(inputIndex)->GradientSlice(frameRange).AddWithRowSliceValuesOf(GradientSlice(frameRange), m_startRowIndices[inputIndex], Inputs(inputIndex)->GetNumRows());
        }

        virtual void /*ComputationNode::*/EvaluateThisNode(const FrameRange & frameRange) override
        {
            for (size_t inputIndex = 0; inputIndex < ChildrenSize(); inputIndex++)
                ValueSlice(frameRange).AssignToRowSliceValuesOf(Inputs(inputIndex)->ValueSlice(frameRange), m_startRowIndices[inputIndex], Inputs(inputIndex)->GetNumRows());
        }

        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            Base::Validate(isFinalValidationPass);
            InferMBLayoutFromInputsForStandardCase();

            size_t numCols = Inputs(0)->GetNumCols();

            // count totalRows and form m_startRowIndices[] array, which is the cumulative sum of matrix heights
            m_startRowIndices.resize(ChildrenSize());
            size_t totalRows = 0;

            for (int i = 0; i < ChildrenSize(); i++)
            {
                if (isFinalValidationPass && Inputs(i)->GetNumCols() != numCols)
                    LogicError("RowStack operation: the input node %ls has different number of columns.", Inputs(i)->NodeName().c_str());

                m_startRowIndices[i] = totalRows;
                totalRows += Inputs(i)->GetNumRows();
            }

            Resize(totalRows, numCols);
            InferImageDimsFromInputs();
        }

        virtual void InferImageDimsFromInputs()
        {
            InferImageDimsFromInput(0, true);
            m_outputImageLayout.height = GetNumRows();

            //WARNING: this node will destroy the image size information from the child
            if (m_inputImageLayout.width * m_inputImageLayout.channels != 1)
                fprintf(stderr, "WARNING: RowStack operation cannot inherit image size information from its child. Image size info is lost.\n");
        }

    private:
        std::vector<size_t> m_startRowIndices;  // start row number in the stacked matrix of each input (child) (cumsum of matrix heights)
    };

    template class RowStackNode<float>;
    template class RowStackNode<double>;

    // -----------------------------------------------------------------------
    // RowRepeatNode (input) -- duplicate row(s) of a matrix multiple times
    // -----------------------------------------------------------------------

    template<class ElemType>
    class RowRepeatNode : public ComputationNode<ElemType>, public NumInputs<1>
    {
        typedef ComputationNode<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"RowRepeat"; }
    public:
        RowRepeatNode(DEVICEID_TYPE deviceId, const wstring & name, size_t numRepeats = 1) :
            Base(deviceId, name),
            m_numRepeat(numRepeats)
        { }

        virtual void CopyTo(const ComputationNodePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const override
        {
            Base::CopyTo(nodeP, newName, flags);
            if (flags & CopyNodeFlags::copyNodeValue)
            {
                auto node = dynamic_pointer_cast<RowRepeatNode<ElemType>>(nodeP);
                node->m_numRepeat = m_numRepeat;
            }
        }

        virtual void SaveToFile(File& fstream) const override
        {
            Base::SaveToFile(fstream);
            fstream << m_numRepeat;
        }

        virtual void LoadFromFile(File& fstream, size_t modelVersion) override
        {
            Base::LoadFromFile(fstream, modelVersion);
            fstream >> m_numRepeat;
        }

        virtual void InferImageDimsFromInputs()
        {
            InferImageDimsFromInput(0, true);
            m_outputImageLayout.height = m_inputImageLayout.height * m_numRepeat;

            // WARNING: this node will destroy the image size information from the child
            if (m_inputImageLayout.width * m_inputImageLayout.channels != 1)
                fprintf(stderr, "WARNING: RowRepeat operation cannot inherit image size information from its child. Image size info is lost.\n");
        }

        virtual void PrintSelfBeforeValidation(bool allowNulls = false) const
        {
            fprintf(stderr, "\nValidating --> %ls = %ls", NodeName().c_str(), OperationName().c_str());

            if (!IsLeaf())
            {
                fprintf(stderr, "(");
                for (size_t i = 0; i<ChildrenSize(); i++)
                {
                    ComputationNodePtr child = Inputs(i);
                    if (i > 0)
                        fprintf(stderr, ", ");

                    if (child == nullptr)
                    {
                        if (allowNulls)
                        {
                            fprintf(stderr, "NULL");
                            continue;
                        }
                        RuntimeError("One of the children is missing.");
                    }

                    fprintf(stderr, "%ls[%lu, %lu]", child->NodeName().c_str(), child->GetNumRows(), child->GetNumCols());
                }

                fprintf(stderr, ", numRepeats=%lu)", m_numRepeat);
            }
        }

        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            Base::Validate(isFinalValidationPass);

            Resize(Inputs(0)->GetNumRows() * m_numRepeat, Inputs(0)->GetNumCols());
            InferMBLayoutFromInputsForStandardCase();
            InferImageDimsFromInputs();
        }

        virtual void /*ComputationNode::*/EvaluateThisNode(const FrameRange & frameRange) override
        {
            if (!isNoop())    // if m_numRepeat == 1 then virtual FunctionValues() will return the child   --TODO: do this as an in-place optimization instead
                ValueSlice(frameRange).AssignRepeatOf(Inputs(0)->ValueSlice(frameRange), m_numRepeat, 1);
        }

        virtual void /*ComputationNode::*/ComputeInputPartial(const size_t /*inputIndex*/, const FrameRange & frameRange) override
        {
            Inputs(0)->GradientSlice(frameRange).AddToRowRepeatValuesOf(GradientSlice(frameRange), m_numRepeat);
        }

        // TODO: Can we remove this const-related duplication as well?
        virtual const Matrix<ElemType>& FunctionValues() const
        {
            if (!isNoop())
                return *m_functionValues;
            else
                return Inputs(0)->FunctionValues();
        }

        virtual Matrix<ElemType>& FunctionValues() 
        {
            if (!isNoop())
                return *m_functionValues;
            else
                return Inputs(0)->FunctionValues();
        }

    private:
        bool isNoop() const { return m_numRepeat == 1; }    // in this case this node does nothing
        size_t m_numRepeat;
    };

    template class RowRepeatNode<float>;
    template class RowRepeatNode<double>;

}}}