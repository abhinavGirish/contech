// 15-745 Project
// Group:
////////////////////////////////////////////////////////////////////////////////

#include "LoopIV.h"

using namespace llvm;
using namespace std;


#include "llvm/ADT/SmallSet.h"
static cl::opt<unsigned>
MaxInc("max-reroll-increment1", cl::init(2048), cl::Hidden,
cl::desc("The maximum increment for loop rerolling"));

namespace llvm{
    bool isCompareUsedByBranch(Instruction *I) {
        auto *TI = I->getParent()->getTerminator();
        if (!isa<BranchInst>(TI) || !isa<CmpInst>(I))
            return false;
        return I->hasOneUse() && TI->getOperand(0) == I;
    }

    DenseMap<Instruction *, int64_t> IVToIncMap;

    vector<Instruction*>invar;
    vector<Value*>def;
    ScalarEvolution *SE;
    Instruction* LoopControlIV;
    SmallInstructionVector PossibleIVs;
    SmallInstructionVector NonLinearIvs;
    SmallInstructionVector DerivedLinearIvs;
    SmallInstructionVector DerivedNonlinearIvs;
    SmallInstructionVector LoopMemoryOps;

    int cnt_GetElementPtrInst = 0;
    int cnt_elided = 0;

    // Check if it is a compare-like instruction whose user is a branch
    bool LoopIV::isLoopControlIV(Loop *L, Instruction *IV) 
    {
        unsigned IVUses = IV->getNumUses();
        if (IVUses != 2 && IVUses != 1)
        {
            return false;
        }

        for (auto *User : IV->users()) 
        {
            int32_t IncOrCmpUses = User->getNumUses();
            bool IsCompInst = isCompareUsedByBranch(cast<Instruction>(User));

            // User can only have one or two uses.
            if (IncOrCmpUses != 2 && IncOrCmpUses != 1)
            {
                return false;
            }

            // Case 1
            if (IVUses == 1) 
            {
              // The only user must be the loop increment.
              // The loop increment must have two uses.
                if (IsCompInst || IncOrCmpUses != 2)
                    return false;
            }

            // Case 2
            if (IVUses == 2 && IncOrCmpUses != 1)
            {
                return false;
            }

            // The users of the IV must be a binary operation or a comparison
            if (auto *BO = dyn_cast<BinaryOperator>(User)) 
            {
                if (BO->getOpcode() == Instruction::Add) 
                {
                    // Loop Increment
                    // User of Loop Increment should be either PHI or CMP
                    for (auto *UU : User->users()) {
                        if (PHINode *PN = dyn_cast<PHINode>(UU)) 
                        {
                            if (PN != IV)
                            {
                                return false;
                            }
                        }
                          // Must be a CMP or an ext (of a value with nsw) then CMP
                        else {
                            Instruction *UUser = dyn_cast<Instruction>(UU);
                            // Skip SExt if we are extending an nsw value
                            // TODO: Allow ZExt too
                            if (BO->hasNoSignedWrap() && UUser && UUser->getNumUses() == 1 &&
                                isa<SExtInst>(UUser))
                            {
                                UUser = dyn_cast<Instruction>(*(UUser->user_begin()));
                            }
                            if (!isCompareUsedByBranch(UUser))
                            {
                                return false;
                            }
                        }
                    }
                } 
                else
                {
                    return false;
                }
              // Compare : can only have one use, and must be branch
            } 
            else if (!IsCompInst)
            {
                return false;
            }
        }
        
        return true;
    }

    const SCEVConstant *LoopIV::getIncrmentFactorSCEV(ScalarEvolution *SE,
        const SCEV *SCEVExpr,
        Instruction &IV) 
    {
        const SCEVMulExpr *MulSCEV = dyn_cast<SCEVMulExpr>(SCEVExpr);

      // If StepRecurrence of a SCEVExpr is a constant (c1 * c2, c2 = sizeof(ptr)),
      // Return c1.
        if (!MulSCEV && IV.getType()->isPointerTy())
        {
            if (const SCEVConstant *IncSCEV = dyn_cast<SCEVConstant>(SCEVExpr)) 
            {
                const PointerType *PTy = cast<PointerType>(IV.getType());
                Type *ElTy = PTy->getElementType();
                const SCEV *SizeOfExpr = SE->getSizeOfExpr(SE->getEffectiveSCEVType(IV.getType()), ElTy);
                
                if (IncSCEV->getValue()->getValue().isNegative()) 
                {
                    const SCEV *NewSCEV = SE->getUDivExpr(SE->getNegativeSCEV(SCEVExpr), SizeOfExpr);
                    return dyn_cast<SCEVConstant>(SE->getNegativeSCEV(NewSCEV));
                } 
                else 
                {
                    return dyn_cast<SCEVConstant>(SE->getUDivExpr(SCEVExpr, SizeOfExpr));
                }
            }
        }

        if (!MulSCEV)
        {
            return nullptr;
        }

      // If StepRecurrence of a SCEVExpr is a c * sizeof(x), where c is constant,
      // Return c.
        const SCEVConstant *CIncSCEV = nullptr;
        for (const SCEV *Operand : MulSCEV->operands()) 
        {
            if (const SCEVConstant *Constant = dyn_cast<SCEVConstant>(Operand)) 
            {
                CIncSCEV = Constant;
            } 
            else if (const SCEVUnknown *Unknown = dyn_cast<SCEVUnknown>(Operand)) 
            {
                Type *AllocTy;
                if (!Unknown->isSizeOf(AllocTy)) break;
            } 
            else 
            {
                return nullptr;
            }
        }
        return CIncSCEV;
    }

    void LoopIV::collectPossibleIVs(Loop *L) 
    {
        int cnt = 0;
        PossibleIVs.clear();
        NonLinearIvs.clear();
        BasicBlock *Header = L->getHeader();
        for (BasicBlock::iterator I = Header->begin(),
            IE = Header->getFirstInsertionPt(); I != IE; ++I, cnt++ ) 
        {

            //errs() << " INST " << *I << "\n";
            if (!isa<PHINode>(I)) 
            {
                continue;
            }

            if (!I->getType()->isIntegerTy() && !I->getType()->isPointerTy()) 
            {
                //errs() << "IS NOT INTEGEER OR POINTER TYPE\n";
                continue;
            }

            const SCEV *S = SE->getSCEV(&*I);
            const SCEVAddRecExpr *SARE = dyn_cast<SCEVAddRecExpr>(S);
            if (SARE) 
            {
                const Loop *CurLoop = SARE->getLoop();
                if (CurLoop == L) 
                {
                    if(SARE->getNumOperands() > 2) 
                    {
                        NonLinearIvs.push_back(&*I);
                        //errs() << "\nNON_LINEAR " << *I << " = " << *SARE << "\n\n" ;
                        //errs() << "Num of operands:" << SARE->getNumOperands() << "\n";
                        for(auto count= SARE->op_begin(); count != SARE->op_end(); count++) 
                        {
                            if(dyn_cast<SCEVConstant>(*count)) 
                            {
                                //errs() << "## " << *dyn_cast<SCEVConstant>(*count) << "\n";
                            }
                        }
                        continue;
                    }
                }
            }

            //errs() << "SCEV " << *SE->getSCEV(&*I) << "\n";
            if (const SCEVAddRecExpr *PHISCEV = dyn_cast<SCEVAddRecExpr>(SE->getSCEV(&*I))) 
            {
                if (PHISCEV->getLoop() != L) 
                {
                    //errs() << "GETLOOP NOT EQUAL L\n";
                    continue;
                }
                if(PHISCEV->isQuadratic()) 
                {
                    //errs() << *I << " = " << *PHISCEV << "IS QUADRATIC\n";
                }

                if (!PHISCEV->isAffine()) 
                {
                    //errs() << "IS NOT AFFINEE\n";
                    continue;
                }
                
                const SCEVConstant *IncSCEV = nullptr;
                if (I->getType()->isPointerTy()) 
                {
                    IncSCEV = getIncrmentFactorSCEV(SE, PHISCEV->getStepRecurrence(*SE), *I);
                    //errs() << "SCEV isPointerTy " << *I << "\n";
                }
                else 
                {
                    IncSCEV = dyn_cast<SCEVConstant>(PHISCEV->getStepRecurrence(*SE));
                    //errs() << "SCEV consant " << *I << "\n";
                }

                if (IncSCEV) 
                {
                    const APInt &AInt = IncSCEV->getValue()->getValue().abs();
                    if (IncSCEV->getValue()->isZero() || AInt.uge(MaxInc))
                        continue;
                    IVToIncMap[&*I] = IncSCEV->getValue()->getSExtValue();

                    //errs() << "LRR: Possible IV: " << *I << " = " << *PHISCEV << "\n";
                    
                    if (isLoopControlIV(L, &*I)) 
                    {
                          //assert(!LoopControlIV && "Found two loop control only IV");
                          //LoopControlIV = &(*I);
                         //errs() << "LRR: Possible loop control only IV: " << *I << " = "
                          //             << *PHISCEV << "\n";
                    } 
                    else 
                    {
                      //errs() << "PossibleIVs " << *I << "\n";
                        PossibleIVs.push_back(&*I);
                    }
                }
            }
        }
    }

    void LoopIV::collectDerivedIVs(Loop *L, SmallInstructionVector IVs, SmallInstructionVector *DerivedIvs)
    {
        DerivedIvs->clear();
        
        for(Loop::block_iterator bb = L->block_begin(); bb != L->block_end(); ++bb) 
        {
            BasicBlock* b = (*bb);

            for(BasicBlock::iterator I = b->begin(); I != b->end(); ++I)
            {
                if (!I->getType()->isIntegerTy() && !I->getType()->isPointerTy()) 
                {
                    //errs() << "IS NOT INTEGEER OR POINTER TYPE\n";
                    continue;
                }
                
                if(std::find(PossibleIVs.begin(), PossibleIVs.end(), (&*I)) != PossibleIVs.end()) 
                {
                    continue;
                }
                if(std::find(NonLinearIvs.begin(), NonLinearIvs.end(), (&*I)) != NonLinearIvs.end()) 
                {
                    continue;
                }

                if (!isa<BinaryOperator>(*I)) 
                {
                    continue;
                }

                Instruction* temp = cast<Instruction>(&*I);
                unsigned cnt = 0;
                bool is_derived = false;
                for(unsigned i = 0; i < temp->getNumOperands(); i++) 
                {
                    Instruction* nl = dyn_cast<Instruction>(&*temp->getOperand(i));
                    if(isa<Argument>(temp->getOperand(i))) 
                    {
                        break;
                    }
                    if(isa<Constant>(temp->getOperand(i)))     //if operand if const
                    {
                        cnt++;
                    }
                    else if(L->isLoopInvariant(temp->getOperand(i)))     //or it is loop invariant
                    {
                        cnt++;
                    }
                    else if(std::find(IVs.begin(), IVs.end(), temp->getOperand(i)) != IVs.end())     //or it is an IV
                    {
                        cnt++;
                        is_derived = true;
                    }
                    else if(nl != NULL) 
                    {
                        //if derived from non-linear
                        if(std::find(IVs.begin(), IVs.end(), nl->getOperand(0)) != IVs.end())     //or it is an IV
                        {
                            cnt++;
                            is_derived = true;
                        }
                        else if(std::find(IVs.begin(), IVs.end(), nl->getOperand(1)) != IVs.end())     //or it is an IV
                        {
                            cnt++;
                            is_derived = true;
                        }

                    }
                }
                if (is_derived && cnt == temp->getNumOperands()) {
                    DerivedIvs->push_back(&*I);
                    //get SCEV
                }
            }
        }
    }

    int LoopIV::collectPossibleMemoryOps(GetElementPtrInst* gepAddr, SmallInstructionVector IVs, bool is_derived)
    {
        int cnt_elided = 0;

        if (gepAddr != NULL) 
        {
            for (auto itG = gep_type_begin(gepAddr), etG = gep_type_end(gepAddr); itG != etG; ++itG) 
            {
                Value* gepI = itG.getOperand();
                Instruction* temp = cast<Instruction>(gepI);
                if (dyn_cast<ConstantInt>(gepI)) 
                {
                    continue;
                }
                else 
                {
                    if(std::find(IVs.begin(), IVs.end(), gepI) != IVs.end()) 
                    {
                        //errs() << "THERE: " << *gepAddr << "\n";    //TODO:remove
                        cnt_elided++;
                    }
                    //traverse the operands
                    else if(!isa <Argument>(gepI)) //isa<CastInst>(gepI)
                    {
                        Value* gepI = temp->getOperand(0);
                        if(std::find(IVs.begin(), IVs.end(), gepI) != IVs.end()) 
                        {
                            //errs() << "FOUND IT 1: " << *gepAddr << "\n";
                            cnt_elided++;
                        }

                        else if(!isa <Argument>(gepI))  //any other operation involving IV and COnst or LI Inst
                        {
                            Instruction* temp = dyn_cast<Instruction>(gepI);
                            bool is_safe = true, is_found = false;
                            if (temp == NULL) 
                            {
                                continue;
                            }
                            
                            int op = 0;
                            if (temp->getNumOperands() == 2) 
                            {
                                for (unsigned i = 0; i < temp->getNumOperands(); i++)
                                {    //assuming 2 operands in 3 add inst
                                    if(temp->getOperand(i) == temp->getOperand(1-i)) 
                                    {
                                        is_safe = false;
                                    }
                                    
                                    if(std::find(IVs.begin(), IVs.end(), temp->getOperand(i)) != IVs.end()) 
                                    {
                                        is_found = true;
                                        op = i;
                                    }
                                }
                                
                                if(is_safe && is_found)     //check for other operand 
                                {
                                    const SCEVAddRecExpr *S = dyn_cast<SCEVAddRecExpr>(SE->getSCEV(&*temp->getOperand(1-op)));
                                    if (S != NULL) 
                                    {
                                        if(!is_derived) 
                                        {
                                            cnt_elided++;
                                            //errs() << "FOUND IT 2 : " << *gepAddr << "\n";
                                        }
                                    }
                                }
                            }
                            else if (temp->getNumOperands() == 1) 
                            {
                                if (std::find(IVs.begin(), IVs.end(), temp->getOperand(0)) != IVs.end()) 
                                {
                                    cnt_elided++;
                                    //errs() << "FOUND IT 3: " << *gepAddr << "\n";
                                }
                            }
                            // case with >= 2
                        }
                    }
                }
            }
        }
        return cnt_elided;
    }

    //bool LoopIV::runOnLoop(Loop *L, LPPassManager &LPM) {
    bool LoopIV::runOnFunction (Function &F) 
    {
        if (!F.isDeclaration()) 
        {
          LoopMemoryOps.clear();
          cnt_GetElementPtrInst = 0;
          cnt_elided = 0;

          //errs() << F;
          
          LoopInfo &LI = *ctThis->getAnalysisLoopInfo(F);
          SE = ctThis->getAnalysisSCEV(F);
          for(LoopInfo::iterator i = LI.begin(), e = LI.end(); i!=e; ++i) 
          {
            Loop *L = *i;

            if (!SE->hasLoopInvariantBackedgeTakenCount(L)) 
            {
                return false;
            }

            // Iterate on subloops of Loop L
            // TODO: check if this will still execute on loop L if it contains no sub loops
            for (auto subL = L->begin(), subLE = L->end(); subL != subLE; ++subL)
            {
                collectPossibleIVs((*subL));
                collectDerivedIVs((*subL), PossibleIVs, &DerivedLinearIvs);
                collectDerivedIVs((*subL), NonLinearIvs, &DerivedNonlinearIvs);
                for(Loop::block_iterator bb = (*subL)->block_begin(); bb != (*subL)->block_end(); ++bb) 
                {
                    BasicBlock* b = (*bb);
                    for(BasicBlock::iterator I = b->begin(); I != b->end(); ++I) 
                    {
                        GetElementPtrInst *gepAddr = NULL;

                        if (LoadInst *li = dyn_cast<LoadInst>(&*I))    
                        {
                            gepAddr = dyn_cast<GetElementPtrInst>(li->getPointerOperand());
                            cnt_GetElementPtrInst++;
                        }
                        else if (StoreInst *si = dyn_cast<StoreInst>(&*I)) 
                        {
                            gepAddr = dyn_cast<GetElementPtrInst>(si->getPointerOperand());
                            cnt_GetElementPtrInst++;
                        }
                        else 
                        {
                            continue;
                        }
                        
                        if(collectPossibleMemoryOps(gepAddr, PossibleIVs, false)) 
                        {
                            cnt_elided++;
                            LoopMemoryOps.push_back(&*I);
                        }
                        else if(collectPossibleMemoryOps(gepAddr, NonLinearIvs, false)) 
                        {
                            cnt_elided++;
                            LoopMemoryOps.push_back(&*I);
                        }
                        else if(collectPossibleMemoryOps(gepAddr, DerivedLinearIvs, true)) 
                        {
                            cnt_elided++;
                            LoopMemoryOps.push_back(&*I);
                        }
                        else if(collectPossibleMemoryOps(gepAddr, DerivedNonlinearIvs, true)) 
                        {
                            cnt_elided++;
                            LoopMemoryOps.push_back(&*I);
                        }
                    }
                }
            }
    #if 0
            outs() << "----------- Print summary for the loop --------------\n";
            
            outs() << "LRR: F[" << Header->getParent()->getName() <<
            "] Loop %" << Header->getName() << " (" <<
            L->getNumBlocks() << " block(s))\n";

              outs() << "LRR: iteration count = " << *IterCount << "\n";

            outs() << "LINEAR INDUCTION VARIABLES:\n";
            for(auto iv = PossibleIVs.begin(); iv != PossibleIVs.end(); ++iv) {
                outs() << **iv << "\t***inc is : " << IVToIncMap[*iv] << "\n";
            }
            
            outs() << "NON-LINEAR INDUCTION VARIABLES:\n";
            for(auto iv = NonLinearIvs.begin(); iv != NonLinearIvs.end(); ++iv) {
                outs() << **iv << "\n";
            }
            
            outs() << "POSSIBLE DERIVED LINEAR INDUCTION VARIABLES:\n";
            for(auto iv = DerivedLinearIvs.begin(); iv != DerivedLinearIvs.end(); ++iv) {
                outs() << **iv << "\n";
            }

            outs() << "POSSIBLE DERIVED NON-LINEAR INDUCTION VARIABLES:\n";
            for(auto iv = DerivedNonlinearIvs.begin(); iv != DerivedNonlinearIvs.end(); ++iv) {
                outs() << **iv << "\n";
            }

            outs() << "cnt_GetElementPtrInst\t" << cnt_GetElementPtrInst << "\n";
            outs() << "cnt_elided\t" << cnt_elided << "\n";

            outs() << "-----------------------------------------------------\n\n\n";
    #endif        
            }
        }
        return false;
    }

    SmallInstructionVector LoopIV:: getLoopMemoryOps() 
    {
        outs() << "cnt_GetElementPtrInst\t" << cnt_GetElementPtrInst << "\n";
        outs() << "cnt_elided\t" << cnt_elided << "\n";
        return LoopMemoryOps;
    }
}