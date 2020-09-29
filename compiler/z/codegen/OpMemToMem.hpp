/*******************************************************************************
 * Copyright (c) 2000, 2020 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at http://eclipse.org/legal/epl-2.0
 * or the Apache License, Version 2.0 which accompanies this distribution
 * and is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following Secondary
 * Licenses when the conditions for such availability set forth in the
 * Eclipse Public License, v. 2.0 are satisfied: GNU General Public License,
 * version 2 with the GNU Classpath Exception [1] and GNU General Public
 * License, version 2 with the OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

#ifndef OPMEMTOMEM_INCL
#define OPMEMTOMEM_INCL

#include <stddef.h>
#include <stdint.h>
#include "codegen/CodeGenerator.hpp"
#include "codegen/InstOpCode.hpp"
#include "codegen/Instruction.hpp"
#include "codegen/Machine.hpp"
#include "codegen/MemoryReference.hpp"
#include "codegen/RegisterPair.hpp"
#include "compile/Compilation.hpp"
#include "env/CompilerEnv.hpp"
#include "env/TRMemory.hpp"
#include "env/jittypes.h"
#include "il/DataTypes.hpp"
#include "il/LabelSymbol.hpp"
#include "infra/Assert.hpp"
#include "z/codegen/S390GenerateInstructions.hpp"

namespace TR { class Node; }
namespace TR { class Register; }
namespace TR { class RegisterDependencyConditions; }
namespace TR { class SymbolReference; }

class MemToMemMacroOp
   {
   public:
      virtual TR::Instruction *generate(TR::Register* dstReg,
                                       TR::Register* srcReg,
                                       TR::Register* tmpReg,
                                       int32_t offset,
                                       TR::Instruction *cursor,
                                       TR::MemoryReference *dstMR = NULL,
                                       TR::MemoryReference *srcMR = NULL)
         {
         TR_ASSERT(offset<=MAXDISP,"MemToMemMacroOp: offset must be less than MAXDISP\n");

         TR::Instruction *cursorBefore = cursor;
         _dstReg = dstReg;
         _srcReg = srcReg;
         _tmpReg = tmpReg;
         _offset = offset;
         _cursor = cursor;
         _dstMR  = dstMR;
         _srcMR  = srcMR;
         _srcRegTemp = NULL;
         _dstRegTemp = NULL;
         _litReg = NULL;
         TR::Compilation *comp = _cg->comp();

         if(cursorBefore == NULL) cursorBefore = comp->cg()->getAppendInstruction();
         _cursor = generateLoop();
         _cursor = generateRemainder();

         // It's possible that no instruction generated by generateLoop and generateRemainder.
         // e.g. zero arraylength, if so, we shouldn't set dependency
         if (_cursor && (cursorBefore != _cursor))
            {
            TR::RegisterDependencyConditions * dependencies = generateDependencies();
            if (dependencies != 0)
               {
               _cursor->setDependencyConditions(dependencies);

               if(_startControlFlow==NULL)
                 {
                 _startControlFlow=cursorBefore->getNext();
                 if(_startControlFlow->getOpCodeValue() == TR::InstOpCode::ASSOCREGS) _startControlFlow=_startControlFlow->getNext();
                 }
               if(_startControlFlow != _cursor)
                 {
                 TR::LabelSymbol * cFlowRegionStart = generateLabelSymbol(_cg);
                 TR::LabelSymbol * cFlowRegionEnd = generateLabelSymbol(_cg);

                 generateS390LabelInstruction(_cg, TR::InstOpCode::LABEL, _rootNode, cFlowRegionStart, dependencies, _startControlFlow->getPrev());
                 cFlowRegionStart->setStartInternalControlFlow();

                 generateS390LabelInstruction(_cg, TR::InstOpCode::LABEL, _rootNode, cFlowRegionEnd, _cursor->getPrev());
                 cFlowRegionEnd->setEndInternalControlFlow();
                 }
               }
            }

         if (_srcRegTemp)
            _cg->stopUsingRegister(_srcRegTemp);
         if (_dstRegTemp)
            _cg->stopUsingRegister(_dstRegTemp);

         return _cursor;
         }

      enum Kind
         {
         IsNotExtended,
         IsMemInit,
         IsMemClear,
         IsMemCmp,
         IsMemCpy,
         IsBitOpMem,
         numKinds
         };

      virtual Kind getKind() { return IsNotExtended; }

      virtual TR::Instruction *generate(TR::Register* dstReg, TR::Register* srcReg)
         {
         return generate(dstReg, srcReg, NULL, 0, NULL);
         }

      virtual TR::Instruction* generate(TR::Register* dstReg)
         {
         return generate(dstReg, dstReg, NULL, 0, NULL);
         }

      virtual TR::Instruction *generate(TR::Register* dstReg, TR::Register* srcReg, TR::Register* tmpReg, TR::Register* itersReg, int32_t offset, TR::Instruction *cursor)
         {
         _itersReg = itersReg;  // We need to set this reg up explicitly when calling here from the prolog code
         return generate(dstReg, srcReg, tmpReg, offset, cursor);
         }

      virtual TR::Instruction *generate(TR::MemoryReference* dstMR, TR::MemoryReference* srcMR)
         {
         return generate(NULL, NULL, NULL, 0, NULL, dstMR, srcMR);
         }

      virtual TR::Instruction *generate(TR::MemoryReference* dstMR)
         {
         return generate(NULL, NULL, NULL, 0, NULL, dstMR, dstMR);
         }

      virtual bool setUseEXForRemainder(bool v)
         {
         return _useEXForRemainder = v;
         }

      virtual bool useEXForRemainder()
         {
         return _useEXForRemainder;
         }

      virtual bool setInRemainder(bool v)
         {
         return _inRemainder = v;
         }

      virtual bool inRemainder()
         {
         return _inRemainder;
         }

   protected:
      MemToMemMacroOp(TR::Node* rootNode, TR::Node* dstNode, TR::Node* srcNode, TR::CodeGenerator * cg, TR::Node * lenNode, TR::Register * itersReg = 0)
         : _rootNode(rootNode), _dstNode(dstNode), _srcNode(srcNode), _cg(cg), _lenNode(lenNode),
           _dstReg(0), _srcReg(0), _itersReg(itersReg), _tmpReg(0), _litReg(0), _srcMR(NULL), _dstMR(NULL), _EXTargetLabel(NULL), _offset(0), _cursor(0), _startControlFlow(0),
           _useEXForRemainder(false), _inRemainder(false)
         {}

      virtual TR::Instruction* generateLoop()=0;
      virtual TR::Instruction* generateRemainder()=0;
      virtual TR::RegisterDependencyConditions* generateDependencies()=0;

      virtual TR::Instruction *genSrcLoadAddress(int32_t offset, TR::Instruction *cursor);
      virtual TR::Instruction *genDstLoadAddress(int32_t offset, TR::Instruction *cursor);

      virtual void generateSrcMemRef(int32_t offset)
         {
         if (_srcMR==NULL)
            {
            if (_srcReg!=NULL)
               {
               _srcMR=new (_cg->trHeapMemory()) TR::MemoryReference(_srcReg, offset, _cg);
               }
            else
               {
               _srcMR= generateS390MemoryReference(_cg, _rootNode, _srcNode , offset, true);
               }
            }
         else
            {
            // Ensure we don't reuse a memref in two different instructions.
            _srcMR= generateS390MemoryReference(*_srcMR, 0, _cg);
            }
         }
      virtual void generateDstMemRef(int32_t offset)
         {
         if (_dstMR==NULL)
            {
            if (_dstReg!=NULL)
               {
               _dstMR=new (_cg->trHeapMemory()) TR::MemoryReference(_dstReg, offset, _cg);
               }
            else
               {
               _dstMR= generateS390MemoryReference(_cg, _rootNode, _dstNode , offset, true);
               }
            }
         else
            {
            // Ensure we don't reuse a memref in two different instructions.
            _dstMR= generateS390MemoryReference(*_dstMR, 0, _cg);
            }
         }
      TR::Node * _lenNode;
      TR::Node* _rootNode;
      TR::Node* _srcNode;
      TR::Node* _dstNode;
      TR::CodeGenerator * _cg;
      TR::Register* _srcReg;
      TR::Register* _dstReg;
      TR::Register* _srcRegTemp;
      TR::Register* _dstRegTemp;
      TR::Register* _itersReg;
      TR::Register* _tmpReg;
      TR::Register* _litReg;
      TR::MemoryReference * _srcMR;
      TR::MemoryReference * _dstMR;
      TR::LabelSymbol * _EXTargetLabel;
      int32_t _offset;
      TR::Instruction* _cursor;
      TR::Instruction* _startControlFlow;
      bool _useEXForRemainder;
      bool _inRemainder;

   /** \brief
    *     Defines the minimum length of the operands that can be encoded by an SS instruction format.
    */
   static const int32_t MIN_LENGTH_FOR_SS_INSTRUCTION = 1;
   };

class MemToMemConstLenMacroOp : public MemToMemMacroOp
   {
   public:
      bool needsLoop() {return _needsLoop; }
      TR::RegisterDependencyConditions *getDependenciesForICF()
         {
         TR_ASSERT(_inNestedICF, "ICF dependencies were not created");
         return _nestedICFDeps;
         }
   protected:
      MemToMemConstLenMacroOp(TR::Node* rootNode, TR::Node* dstNode, TR::Node* srcNode, TR::CodeGenerator * cg, int64_t length,
                              TR::Register * itersReg=0, TR::InstOpCode::Mnemonic op=TR::InstOpCode::BAD, bool inNestedICF=false)
        : MemToMemMacroOp(rootNode, dstNode, srcNode, cg,NULL, itersReg), _length(length), _maxCopies(16), _opcode(op), _needDep(true), _inNestedICF(inNestedICF), _nestedICFDeps(NULL)
         {
         uint64_t len = (uint64_t)length;
         uint64_t largeCopies = (len == 0) ? 0 : (len - 1) / 256;

         // Copy early exit logic from MemToMemConstLenMacroOp::generateLoop()
         _needsLoop =  !(largeCopies < (uint64_t)_maxCopies);
         }
      virtual TR::Instruction* generateLoop();
      virtual TR::Instruction* generateInstruction(int32_t offset, int64_t length,TR::Instruction* cursor);
      virtual TR::Instruction* generateRemainder();
      bool noLoop() {return !_needsLoop; }

      void setDependencies(bool needDep)
         {
         _needDep = needDep;
         return;
         }

      bool needDependencies()
         {
         return _needDep;
         }

      int64_t _length;
      int32_t _maxCopies; ///< this can be modified - it needs to be less than 16 (addressability)
      TR::InstOpCode::Mnemonic _opcode;
      bool _needsLoop;
      bool _inNestedICF;
      TR::RegisterDependencyConditions *_nestedICFDeps;
   private:
      bool _needDep; ///< Indicates that dependencies are not needed as the copy is done in a non-loop fashion.

   };

class MemToMemVarLenMacroOp : public MemToMemMacroOp
   {
   protected:
      MemToMemVarLenMacroOp(TR::Node* rootNode, TR::Node* dstNode, TR::Node* srcNode, TR::CodeGenerator * cg, TR::Register* regLen, TR::Node * lenNode, bool lengthMinusOne=false, TR::InstOpCode::Mnemonic opcode = TR::InstOpCode::MVC, TR::Register * itersReg = 0, TR::Register * raReg = 0)
         : MemToMemMacroOp(rootNode, dstNode, srcNode, cg, lenNode, itersReg), _regLen(regLen), _raReg(raReg), _doneLabel(0), _opcode(opcode), _lengthMinusOne(lengthMinusOne)
         {}
      virtual TR::Instruction* generateLoop();
      virtual TR::Instruction* generateRemainder();
      virtual intptr_t getHelper()=0;
      virtual TR::SymbolReference* getHelperSymRef()=0;
      virtual TR::Instruction* generateInstruction(int32_t offset, int64_t length)=0;
      TR::Register* _regLen;
      TR::Register* _raReg;
      TR::LabelSymbol *_doneLabel; ///< set and used for the EXForRemainder case
      TR::InstOpCode::Mnemonic _opcode;
      bool _lengthMinusOne;
   private:
   };

class MemInitConstLenMacroOp : public MemToMemConstLenMacroOp
   {
   public:
      MemInitConstLenMacroOp(TR::Node* rootNode, TR::Node* dstNode, TR::CodeGenerator * cg, int64_t length, TR::Register* initReg)
         : MemToMemConstLenMacroOp(rootNode, dstNode, dstNode, cg, length), _initReg(initReg), _useByteVal(false)
      {}
      MemInitConstLenMacroOp(TR::Node* rootNode, TR::Node* dstNode, TR::CodeGenerator * cg, int64_t length, int8_t byteVal)
         : MemToMemConstLenMacroOp(rootNode, dstNode, dstNode, cg, length), _initReg(NULL), _useByteVal(true), _byteVal(byteVal)
      {}
   protected:
      virtual TR::Instruction* generateInstruction(int32_t offset, int64_t length,TR::Instruction* cursor);
      virtual TR::RegisterDependencyConditions* generateDependencies();
      virtual TR::Instruction* generateLoop();
      virtual TR::Instruction* generateRemainder();
   private:
      TR::Register* _initReg;
      bool _useByteVal;
      int8_t _byteVal;
   };

class MemClearConstLenMacroOp : public MemToMemConstLenMacroOp
   {
   public:
      MemClearConstLenMacroOp(TR::Node* rootNode, TR::Node* dstNode, TR::CodeGenerator * cg, int64_t length)
         : MemToMemConstLenMacroOp(rootNode, dstNode, dstNode, cg, length)
      {}
      MemClearConstLenMacroOp(TR::Node* dstNode, int64_t length, TR::CodeGenerator * cg)
         : MemToMemConstLenMacroOp(dstNode, dstNode, dstNode, cg, length)
      {}
   protected:
      virtual TR::Instruction* generateInstruction(int32_t offset, int64_t length,TR::Instruction* cursor);
      virtual TR::RegisterDependencyConditions* generateDependencies();
   private:
   };

class MemCmpConstLenMacroOp : public MemToMemConstLenMacroOp
   {
   public:
      MemCmpConstLenMacroOp(TR::Node* rootNode, TR::Node* dstNode, TR::Node* srcNode, TR::CodeGenerator * cg, int64_t length)
         : MemToMemConstLenMacroOp(rootNode, dstNode, srcNode, cg, length)
      {
      _falseLabel = generateLabelSymbol(cg);
      _trueLabel  = generateLabelSymbol(cg);
      _doneLabel  = generateLabelSymbol(cg);
      }
      TR::Register * resultReg() { return _resultReg; }
      virtual TR::Instruction *generate(TR::Register* dstReg, TR::Register* srcReg, TR::Register* tmpReg, int32_t offset, TR::Instruction *cursor);
      virtual TR::Instruction *generate(TR::Register* dstReg, TR::Register* srcReg)
         {
         return generate(dstReg, srcReg, NULL, 0, NULL);
         }

   protected:
      virtual TR::Instruction* generateInstruction(int32_t offset, int64_t length, TR::Instruction* cursor);
      virtual TR::RegisterDependencyConditions* generateDependencies();
      TR::LabelSymbol *_falseLabel;
      TR::LabelSymbol *_trueLabel;
      TR::LabelSymbol *_doneLabel;
      TR::Register *_resultReg;
   private:
   };

class MemCmpConstLenSignMacroOp : public MemCmpConstLenMacroOp
   {
   public:
      MemCmpConstLenSignMacroOp(TR::Node* rootNode, TR::Node* dstNode, TR::Node* srcNode, TR::CodeGenerator * cg, int64_t length)
         : MemCmpConstLenMacroOp(rootNode, dstNode, srcNode, cg, length)
      {
      _gtLabel     = generateLabelSymbol(cg);
      }
      virtual TR::Instruction *generate(TR::Register* dstReg, TR::Register* srcReg, TR::Register* tmpReg, int32_t offset, TR::Instruction *cursor);
      virtual TR::Instruction *generate(TR::Register* dstReg, TR::Register* srcReg)
         {
         return generate(dstReg, srcReg, NULL, 0, NULL);
         }

   protected:
      TR::LabelSymbol *_gtLabel;
   };

class MemCpyConstLenMacroOp : public MemToMemConstLenMacroOp
   {
   public:
      MemCpyConstLenMacroOp(TR::Node* rootNode, TR::Node* dstNode, TR::Node* srcNode, TR::CodeGenerator * cg, int64_t length, TR::Register * itersReg=0, bool inNestedICF=false)
         : MemToMemConstLenMacroOp(rootNode, dstNode, srcNode, cg, length, itersReg, TR::InstOpCode::MVC, inNestedICF)
         {}
      MemCpyConstLenMacroOp(TR::Node* dstNode, TR::Node* srcNode, int64_t length, TR::CodeGenerator * cg, TR::Register * itersReg=0, bool inNestedICF=false)
         : MemToMemConstLenMacroOp(dstNode, dstNode, srcNode, cg, length, itersReg, TR::InstOpCode::MVC, inNestedICF)
         {}

   protected:
      virtual TR::RegisterDependencyConditions* generateDependencies();
   };

class BitOpMemConstLenMacroOp : public MemToMemConstLenMacroOp
   {
   public:
      BitOpMemConstLenMacroOp(TR::Node* rootNode, TR::Node* dstNode, TR::Node* srcNode, TR::CodeGenerator * cg, TR::InstOpCode::Mnemonic opcode, int64_t length)
         : MemToMemConstLenMacroOp(rootNode, dstNode, srcNode, cg, length, NULL, opcode)
         {}
      BitOpMemConstLenMacroOp(TR::Node* dstNode, TR::Node* srcNode, TR::CodeGenerator * cg, TR::InstOpCode::Mnemonic opcode, int64_t length)
         : MemToMemConstLenMacroOp(dstNode, dstNode, srcNode, cg, length, NULL, opcode)
         {}
   protected:
      virtual TR::RegisterDependencyConditions* generateDependencies();
   private:
   };

class MemInitVarLenMacroOp : public MemToMemVarLenMacroOp
   {
   public:
      MemInitVarLenMacroOp(TR::Node* rootNode, TR::Node* dstNode, TR::CodeGenerator * cg, TR::Register* regLen, TR::Register* initReg, TR::Node * lenNode, bool lengthMinusOne=false)
         : MemToMemVarLenMacroOp(rootNode, dstNode, dstNode, cg, regLen, lenNode, lengthMinusOne), _initReg(initReg), _useByteVal(false), _firstByteInitialized(false), _litPoolReg(NULL)
         {}
      MemInitVarLenMacroOp(TR::Node* rootNode, TR::Node* dstNode, TR::CodeGenerator * cg, TR::Register* regLen, int8_t byteVal, TR::Node * lenNode, bool lengthMinusOne=false)
         : MemToMemVarLenMacroOp(rootNode, dstNode, dstNode, cg, regLen, lenNode, lengthMinusOne), _initReg(NULL), _useByteVal(true), _byteVal(byteVal), _firstByteInitialized(false), _litPoolReg(NULL)
         {}

   protected:
      virtual TR::Instruction* generateInstruction(int32_t offset, int64_t length);
      virtual intptr_t getHelper();
      virtual TR::SymbolReference* getHelperSymRef();
      virtual TR::RegisterDependencyConditions* generateDependencies();
      virtual TR::Instruction* generateRemainder();
      virtual Kind getKind() { return IsMemInit; }
   private:
      TR::Register* _initReg;
      TR::Register *_litPoolReg;
      bool _useByteVal;
      bool _firstByteInitialized;
      int8_t _byteVal;
      bool checkLengthAfterLoop() { return true; }
   };

class MemClearVarLenMacroOp : public MemToMemVarLenMacroOp
   {
   public:
      MemClearVarLenMacroOp(TR::Node* rootNode, TR::Node* dstNode, TR::CodeGenerator * cg, TR::Register* regLen, TR::Node * lenNode, bool lengthMinusOne=false)
         : MemToMemVarLenMacroOp(rootNode, dstNode, dstNode, cg, regLen, lenNode, lengthMinusOne)
         {}
   protected:
      virtual TR::Instruction* generateInstruction(int32_t offset, int64_t length);
      virtual intptr_t getHelper();
      virtual TR::SymbolReference* getHelperSymRef();
      virtual TR::RegisterDependencyConditions* generateDependencies();
      virtual TR::Instruction* generateRemainder();
      virtual Kind getKind() { return IsMemClear; }
   private:
   };

class MemCmpVarLenMacroOp : public MemToMemVarLenMacroOp
   {
   public:
      MemCmpVarLenMacroOp(TR::Node* rootNode, TR::Node* dstNode, TR::Node* srcNode, TR::CodeGenerator * cg, TR::Register* regLen, TR::Node * lenNode, bool lengthMinusOne=false, TR::LabelSymbol * doneLabel = NULL)
         : MemToMemVarLenMacroOp(rootNode, dstNode, srcNode, cg, regLen, lenNode, lengthMinusOne, TR::InstOpCode::CLC)
         {
         _litPoolReg = NULL;
         _falseLabel = generateLabelSymbol(cg);
         _trueLabel  = generateLabelSymbol(cg);
         if (!doneLabel)
            _doneLabel = generateLabelSymbol(cg);
         else
            _doneLabel = doneLabel;
         }
      virtual TR::Instruction * generate(TR::Register* dstReg, TR::Register* srcReg, TR::Register* tmpReg, int32_t offset, TR::Instruction *cursor);
      virtual TR::Instruction *generate(TR::Register* dstReg, TR::Register* srcReg)
         {
         return generate(dstReg, srcReg, NULL, 0, NULL);
         }
      TR::Register * resultReg() { return _resultReg; }
   protected:
      virtual TR::Instruction* generateInstruction(int32_t offset, int64_t length);
      virtual intptr_t getHelper();
      virtual TR::SymbolReference* getHelperSymRef();
      virtual TR::RegisterDependencyConditions* generateDependencies();
      virtual Kind getKind() { return IsMemCmp; }
      TR::LabelSymbol *_falseLabel;
      TR::LabelSymbol *_trueLabel;
      TR::LabelSymbol *_doneLabel;
      TR::Register *_resultReg;
      TR::Register *_litPoolReg;
   private:
   };

class MemCmpVarLenSignMacroOp : public MemCmpVarLenMacroOp
   {
   public:
      MemCmpVarLenSignMacroOp(TR::Node* rootNode, TR::Node* dstNode, TR::Node* srcNode, TR::CodeGenerator * cg, TR::Register* regLen, TR::Node * lenNode)
         : MemCmpVarLenMacroOp(rootNode, dstNode, srcNode, cg, regLen, lenNode)
         {
         _gtLabel     = generateLabelSymbol(cg);
         }
      virtual TR::Instruction * generate(TR::Register* dstReg, TR::Register* srcReg, TR::Register* tmpReg, int32_t offset, TR::Instruction *cursor);
      virtual TR::Instruction *generate(TR::Register* dstReg, TR::Register* srcReg)
         {
         return generate(dstReg, srcReg, NULL, 0, NULL);
         }
      virtual Kind getKind() { return IsMemCmp; }
   protected:
      TR::LabelSymbol *_gtLabel;
   };

class MemToMemTypedMacroOp
   {
   public:
      TR_ALLOC(TR_Memory::MemoryReference)
      virtual TR::Instruction *generate(TR::Register* dstReg, TR::Register* srcReg, TR::Register* strideReg,bool applyDepLocally=true)
         {
         TR::Instruction *cursor;
         _srcReg = srcReg;
         _startReg = dstReg;
         _strideReg = strideReg;
         _applyDepLocally = applyDepLocally;

         _endReg   = _cg->allocateRegister();
         _bxhReg   = _cg->allocateConsecutiveRegisterPair(_startReg, _strideReg);
         cursor = generateLoop();
         _cg->stopUsingRegister(_endReg);
         _cg->stopUsingRegister(_bxhReg);
         return cursor;
         }

      virtual TR::Instruction* generate(TR::Register* dstReg, TR::Register* strideReg, bool applyDepLocally=true)
         {
         return generate(dstReg, dstReg, strideReg, applyDepLocally);
         }
      TR::RegisterDependencyConditions* getDependencies() { return _macroDependencies; }

   protected:
      MemToMemTypedMacroOp(TR::Node* rootNode, TR::Node* dstNode, TR::Node* srcNode, TR::CodeGenerator * cg, TR::DataType destType, TR::Node * lenNode, bool isForward)
         : _rootNode(rootNode), _dstNode(dstNode), _srcNode(srcNode), _cg(cg), _destType(destType),
           _startReg(0), _endReg(0), _srcReg(0), _bxhReg(0), _strideReg(0), _macroDependencies(NULL), _lenNode(lenNode), _isForward(isForward)
         {}

      virtual TR::Instruction* generateLoop()=0;
      virtual void createLoopDependencies(TR::Instruction* cursor)=0;

      TR::Node * _lenNode;
      TR::Node* _rootNode;
      TR::Node* _srcNode;
      TR::Node* _dstNode;
      TR::CodeGenerator * _cg;
      TR::RegisterPair* _bxhReg;
      TR::Register* _srcReg;
      TR::Register* _startReg;
      TR::Register* _endReg;
      TR::Register* _strideReg;
      TR::DataType _destType;
      bool _applyDepLocally;
      bool _isForward;
      TR::RegisterDependencyConditions* _macroDependencies;

   private:
   };

class MemCpyVarLenMacroOp : public MemToMemVarLenMacroOp
   {
   public:
      MemCpyVarLenMacroOp(TR::Node* rootNode, TR::Node* dstNode, TR::Node* srcNode, TR::CodeGenerator * cg, TR::Register* regLen, TR::Node * lenNode, bool lengthMinusOne=false, TR::Register * itersReg=0, TR::Register * raReg=0)
         : MemToMemVarLenMacroOp(rootNode, dstNode, srcNode, cg, regLen, lenNode, lengthMinusOne, TR::InstOpCode::MVC, itersReg, raReg)
         {}
   protected:
      virtual TR::Instruction* generateInstruction(int32_t offset, int64_t length);
      virtual intptr_t getHelper();
      virtual TR::SymbolReference* getHelperSymRef();
      virtual TR::RegisterDependencyConditions* generateDependencies();
      virtual Kind getKind() { return IsMemCpy; }
   private:
   };

class BitOpMemVarLenMacroOp : public MemToMemVarLenMacroOp
   {
   public:
      BitOpMemVarLenMacroOp(TR::Node* rootNode, TR::Node* dstNode, TR::Node* srcNode, TR::CodeGenerator * cg, TR::InstOpCode::Mnemonic opcode, TR::Register* regLen, TR::Node * lenNode, bool lengthMinusOne=false)
         : MemToMemVarLenMacroOp(rootNode, dstNode, srcNode, cg, regLen, lenNode, lengthMinusOne, opcode)
         {}
   protected:
      virtual TR::Instruction* generateInstruction(int32_t offset, int64_t length);
      virtual intptr_t getHelper();
      virtual TR::SymbolReference* getHelperSymRef();
      virtual TR::RegisterDependencyConditions* generateDependencies();
      virtual Kind getKind() { return IsBitOpMem; }
   private:
   };

class MemToMemTypedVarLenMacroOp : public MemToMemTypedMacroOp
   {
   public:
   protected:
      MemToMemTypedVarLenMacroOp(TR::Node* rootNode, TR::Node* dstNode, TR::Node* srcNode, TR::CodeGenerator * cg, TR::DataType destType, TR::Register* lenReg, TR::Node * lenNode, bool isForward = false)
         : MemToMemTypedMacroOp(rootNode, dstNode, srcNode, cg, destType, lenNode, isForward), _lenReg(lenReg)
         {}
      virtual TR::Instruction* generateLoop();
      virtual TR::Instruction* generateInstruction()=0;

      int32_t strideSize();
      int32_t shiftSize();
      int32_t numCoreDependencies();
      TR::RegisterDependencyConditions* addCoreDependencies(TR::RegisterDependencyConditions* baseDependencies);
      TR::Register* _lenReg;

   private:
   };

class MemInitVarLenTypedMacroOp : public MemToMemTypedVarLenMacroOp
   {
   public:
      MemInitVarLenTypedMacroOp(TR::Node* rootNode, TR::Node* dstNode, TR::CodeGenerator * cg, TR::DataType destType, TR::Register* lenReg, TR::Register* initReg, TR::Node * lenNode, bool isForward = false)
         : MemToMemTypedVarLenMacroOp(rootNode, dstNode, dstNode, cg, destType, lenReg, lenNode, isForward), _initReg(initReg)
         {}
   protected:
      virtual TR::Instruction* generateInstruction();
      virtual void createLoopDependencies(TR::Instruction* cursor);

   private:
      TR::Register* _initReg;
   };

class MemCpyVarLenTypedMacroOp : public MemToMemTypedVarLenMacroOp
   {
   public:
      MemCpyVarLenTypedMacroOp(TR::Node* rootNode, TR::Node* dstNode, TR::Node* srcNode, TR::CodeGenerator * cg, TR::DataType destType, TR::Register* lenReg, TR::Node * lenNode, bool needsGuardedLoad, bool isForward = false)
         : _needsGuardedLoad(needsGuardedLoad),
           MemToMemTypedVarLenMacroOp(rootNode, dstNode, srcNode, cg, destType, lenReg, lenNode, isForward)
         {
         allocWorkReg();
         }

      void cleanUpReg()
         {
         _cg->stopUsingRegister(_workReg);
         }

   protected:
      virtual TR::Instruction* generateInstruction();
      virtual void createLoopDependencies(TR::Instruction* cursor);

   private:
      void allocWorkReg();
      TR::Register* _workReg;
      bool          _needsGuardedLoad;
   };

class MemCpyAtomicMacroOp: public MemToMemTypedVarLenMacroOp
   {
public:

   MemCpyAtomicMacroOp(TR::Node* rootNode, TR::Node* dstNode, TR::Node* srcNode, TR::CodeGenerator * cg, TR::DataType destType, TR::Register* lenReg, TR::Node * lenNode, bool isForward=false, bool unroll=false, int32_t constLength=-1);
   // Add getters for registers, so that can be shared with MVC routine in arraycopyEvaluator
   TR::Register * getAlignedReg() { return _alignedReg; };
   TR::Register * getWorkReg() { return _workReg; };

   TR::Instruction* generateLoop();
   TR::Instruction * generateConstLoop(TR::InstOpCode::Mnemonic, TR::InstOpCode::Mnemonic);
   TR::Instruction * generateSTXLoop(int32_t, TR::InstOpCode::Mnemonic, TR::InstOpCode::Mnemonic, bool unroll);
   TR::Instruction * generateSTXLoopLabel(TR::LabelSymbol *, TR::LabelSymbol *, int32_t, TR::InstOpCode::Mnemonic, TR::InstOpCode::Mnemonic);
   TR::Instruction * generateOneSTXthenSTYLoopLabel(TR::LabelSymbol *, TR::LabelSymbol *, int32_t, TR::InstOpCode::Mnemonic, TR::InstOpCode::Mnemonic, int32_t, TR::InstOpCode::Mnemonic, TR::InstOpCode::Mnemonic);

   void cleanUpReg()
      {
      if (_trace)
         traceMsg(_cg->comp(), "MemCpyAtomicMacroOp: cleanUpReg\n");
      _cg->stopUsingRegister(_workReg);
      _cg->stopUsingRegister(_alignedReg);

      if (_unroll && _isForward)
         {
         if (_unrollFactor)
            {
            switch (_unrollFactor)
               {
            case 8:
               _cg->stopUsingRegister(_workReg8);
            case 7:
               _cg->stopUsingRegister(_workReg7);
            case 6:
               _cg->stopUsingRegister(_workReg6);
            case 5:
               _cg->stopUsingRegister(_workReg5);
            case 4:
               _cg->stopUsingRegister(_workReg4);
            case 3:
               _cg->stopUsingRegister(_workReg3);
            case 2:
               _cg->stopUsingRegister(_workReg2);
               }
            }
         }
      }

protected:
   virtual TR::Instruction* generateInstruction(TR::InstOpCode::Mnemonic , TR::InstOpCode::Mnemonic , TR::Register * , int32_t );
   virtual TR::Instruction* generateInstruction( );
   virtual void createLoopDependencies(TR::Instruction* cursor);

private:
   bool _unroll;
   bool _trace;
   int32_t _constLength;

   int32_t _unrollFactor;
   void allocWorkReg();
   TR::Register* _workReg;
   TR::Register* _alignedReg;

   TR::Register* _workReg2;
   TR::Register* _workReg3;
   TR::Register* _workReg4;
   TR::Register* _workReg5;
   TR::Register* _workReg6;
   TR::Register* _workReg7;
   TR::Register* _workReg8;
   };

#endif
