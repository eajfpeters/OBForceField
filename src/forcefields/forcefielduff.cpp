/**********************************************************************
forcefielduff.cpp - UFF force field.
 
Copyright (C) 2007-2008 by Geoffrey Hutchison
Some portions Copyright (C) 2006-2008 by Tim Vandermeersch

This file is part of the Open Babel project.
For more information, see <http://openbabel.sourceforge.net/>
 
This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation version 2 of the License.
 
This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
***********************************************************************/

#include <openbabel/babelconfig.h>
#include <openbabel/mol.h>
#include <openbabel/locale.h>

#include "forcefielduff.h"


using namespace std;

// This implementation was created based on open code and reference websites:
// http://towhee.sourceforge.net/forcefields/uff.html
// http://rdkit.org/
// http://franklin.chm.colostate.edu/mmac/uff.html
// (for the last, use the Wayback Machine: http://www.archive.org/

// As well, the main UFF paper:
// Rappe, A. K., et. al.; J. Am. Chem. Soc. (1992) 114(25) p. 10024-10035.

namespace OpenBabel
{
  template<bool gradients>
  double OBForceFieldUFF::E_Bond()
  {
    vector<OBFFBondCalculationUFF>::iterator i;
    double energy = 0.0;
        
    IF_LOGLVL_HIGH {
      Log("\nB O N D   S T R E T C H I N G\n\n");
      Log("ATOM TYPES  BOND    BOND       IDEAL       FORCE\n");
      Log(" I      J   TYPE   LENGTH     LENGTH     CONSTANT      DELTA      ENERGY\n");
      Log("------------------------------------------------------------------------\n");
    }
 
    unsigned int idxA, idxB;
    double rab, delta, e;
    Eigen::Vector3d Fa, Fb;
    for (i = _bondcalculations.begin(); i != _bondcalculations.end(); ++i) {
      idxA = i->a->GetIdx() - 1;
      idxB = i->b->GetIdx() - 1;

      if (gradients) {
        rab = VectorBondDerivative(GetPositions()[idxA], GetPositions()[idxB], Fa, Fb);
      } else {
        const Eigen::Vector3d ab = GetPositions()[idxA] - GetPositions()[idxB];
        rab = ab.norm();
      }

      delta = rab - i->r0; // we pre-compute the r0 below
      const double delta2 = delta * delta;
      e = i->kb * delta2; // we fold the 1/2 into kb below

      if (gradients) {
        const double dE = 2.0 * i->kb * delta;
        Fa *= dE;
        Fb *= dE;
        GetGradients()[idxA] += Fa;
        GetGradients()[idxB] += Fb;
      }
 
      energy += e;
      
      IF_LOGLVL_HIGH {
        snprintf(_logbuf, BUFF_SIZE, "%-5s %-5s  %4.2f%8.3f   %8.3f     %8.3f   %8.3f   %8.3f\n",
                (*i).a->GetType(), (*i).b->GetType(), 
                (*i).bt, rab, (*i).r0, (*i).kb, delta, e);
        Log(_logbuf);
      }
    }
    
    IF_LOGLVL_MEDIUM {
      snprintf(_logbuf, BUFF_SIZE, "     TOTAL BOND STRETCHING ENERGY = %8.3f %s\n",  energy, GetUnit().c_str());
      Log(_logbuf);
    }
    return energy;
  }
  
  template<bool gradients>
  double OBForceFieldUFF::E_Angle()
  {
    vector<OBFFAngleCalculationUFF>::iterator i;
    double energy = 0.0;

    IF_LOGLVL_HIGH {
      Log("\nA N G L E   B E N D I N G\n\n");
      Log("ATOM TYPES       VALENCE     IDEAL      FORCE\n");
      Log(" I    J    K      ANGLE      ANGLE     CONSTANT      DELTA      ENERGY\n");
      Log("-----------------------------------------------------------------------------\n");
    }
    
    unsigned int idxA, idxB, idxC;
    double theta, e;   
    Eigen::Vector3d Fa, Fb, Fc;
    for (i = _anglecalculations.begin(); i != _anglecalculations.end(); ++i) {
      idxA = i->a->GetIdx() - 1;
      idxB = i->b->GetIdx() - 1;
      idxC = i->c->GetIdx() - 1;
      
      if (gradients) {
        theta = VectorAngleDerivative(GetPositions()[idxA], GetPositions()[idxB], 
            GetPositions()[idxC], Fa, Fb, Fc) * DEG_TO_RAD;
      } else {
        const Eigen::Vector3d ab = GetPositions()[idxA] - GetPositions()[idxB];
        const Eigen::Vector3d bc = GetPositions()[idxC] - GetPositions()[idxB];
        theta = VectorAngle(ab, bc) * DEG_TO_RAD;
      }

      if (!isfinite(theta))
        theta = 0.0; // doesn't explain why GetAngle is returning NaN but solves it for us
    
      const double cosT = cos(theta);
      switch (i->coord) {
      case 1: // sp -- linear case, minima at 180 degrees, max (amplitude 2*ka) at 0, 360
        // Fixed typo from Rappe paper (i.e., it's NOT 1 - cosT)
        e = i->ka * (1.0 + cosT);
        break;
      case 2: // sp2 -- trigonal planar, min at 120, 240, max at 0, 360 (amplitude 2*ka)
        // Rappe form: (1 - cos 3*theta) -- minima at 0, 360 (bad...)
        e = (i->ka/4.5) * (1.0 + (1.0 + cosT)*(4.0*cosT));
        break;
      case 4: // square planar // min at 90, 180, 270, max at 0, 360 (amplitude 2*ka)
      case 6: // octahedral
        // Rappe form: (1 - cos 4*theta) -- minima at 0, 360 (bad...)
        e = i->ka * (1.0 + cosT)*cosT*cosT;
        break;
      default: // general (sp3) coordination
        e = i->ka*(i->c0 + i->c1*cosT + i->c2*(2.0*cosT*cosT - 1.0)); // use cos 2t = (2cos^2 - 1)
      }  
        
      if (gradients) {
        const double sinT = sin(theta);
        double dE;
        
        switch (i->coord) {
        case 1: // sp -- linear case
          dE = -(i->ka * sinT);
          break;
        case 2: // sp2 -- trigonal planar
          dE = -(i->ka*4.0/4.5) * (sinT + sin(2.0*theta));
          break;
        case 4: // square planar
        case 6: // octahedral
          dE = - (i->ka * cosT * (2.0 + 3.0 * cosT) * sinT);
          break;
        default: // general (sp3) coordination
          dE = - (i->ka * (i->c1*sinT + 2.0 * i->c2*sin(2.0 * theta)));
        }
      
        Fa *= dE; 
        Fb *= dE; 
        Fc *= dE;
        GetGradients()[idxA] += Fa;
        GetGradients()[idxB] += Fb;
        GetGradients()[idxC] += Fc;
      }
 
      energy += e;
      
      IF_LOGLVL_HIGH {
        snprintf(_logbuf, BUFF_SIZE, "%-5s %-5s %-5s%8.3f  %8.3f     %8.3f   %8.3f\n", 
            (*i).a->GetType(), (*i).b->GetType(), (*i).c->GetType(), 
            theta * RAD_TO_DEG, (*i).theta0, (*i).ka, e);
        Log(_logbuf);
      }
    }
 
    IF_LOGLVL_MEDIUM {
      snprintf(_logbuf, BUFF_SIZE, "     TOTAL ANGLE BENDING ENERGY = %8.3f %s\n", energy, GetUnit().c_str());
      Log(_logbuf);
    }
    return energy;
  }
  
  template<bool gradients>
  double OBForceFieldUFF::E_Torsion() 
  {
    vector<OBFFTorsionCalculationUFF>::iterator i;
    double energy = 0.0;
 
    IF_LOGLVL_HIGH {
      Log("\nT O R S I O N A L\n\n");
      Log("----ATOM TYPES-----    FORCE         TORSION\n");
      Log(" I    J    K    L     CONSTANT        ANGLE         ENERGY\n");
      Log("----------------------------------------------------------------\n");
    }
    
    unsigned int idxA, idxB, idxC, idxD;
    double tor, e;
    Eigen::Vector3d Fa, Fb, Fc, Fd;
    for (i = _torsioncalculations.begin(); i != _torsioncalculations.end(); ++i) {
      idxA = i->a->GetIdx() - 1;
      idxB = i->b->GetIdx() - 1;
      idxC = i->c->GetIdx() - 1;
      idxD = i->d->GetIdx() - 1;
      
      if (gradients) {
        tor = VectorTorsionDerivative(GetPositions()[idxA], GetPositions()[idxB],
            GetPositions()[idxC], GetPositions()[idxD], Fa, Fb, Fc, Fd);
        if (!isfinite(tor))
          tor = 1.0e-3;
        tor *= DEG_TO_RAD;
      } else {
        Eigen::Vector3d vab, vbc, vcd, abbc, bccd;
        vab = GetPositions()[idxA] - GetPositions()[idxB];
        vbc = GetPositions()[idxB] - GetPositions()[idxC];
        vcd = GetPositions()[idxC] - GetPositions()[idxD];
        abbc = vab.cross(vbc);
        bccd = vbc.cross(vcd);

        double dotAbbcBccd = abbc.dot(bccd);
        tor = acos(dotAbbcBccd / (abbc.norm() * bccd.norm()));
        if (IsNearZero(dotAbbcBccd) || !isfinite(tor)) { // stop any NaN or infinity
          tor = 1.0e-3; // rather than NaN
        }
        else if (dotAbbcBccd > 0.0) {
          tor = -tor;
        }
      }

      const double cosine = cos(tor * i->n);
      e = i->V * (1.0 - i->cosNPhi0*cosine);
    
      if (gradients) {
        const double dE = -(i->V * i->n * i->cosNPhi0 * sin(i->n * tor));
        Fa *= dE;
        Fb *= dE;
        Fc *= dE;
        Fd *= dE;
        GetGradients()[idxA] += Fa;
        GetGradients()[idxB] += Fb;
        GetGradients()[idxC] += Fc;
        GetGradients()[idxD] += Fd;
      }
  
      energy += e;
      
      IF_LOGLVL_HIGH {
        snprintf(_logbuf, BUFF_SIZE, "%-5s %-5s %-5s %-5s%6.3f       %8.3f     %8.3f\n",
                (*i).a->GetType(), (*i).b->GetType(), 
                (*i).c->GetType(), (*i).d->GetType(), (*i).V, 
                tor * RAD_TO_DEG, e);
        Log(_logbuf);
      }
    }

    IF_LOGLVL_MEDIUM {
      snprintf(_logbuf, BUFF_SIZE, "     TOTAL TORSIONAL ENERGY = %8.3f %s\n", energy, GetUnit().c_str());
      Log(_logbuf);
    }

    return energy;
  }

  /*
  //  a
  //   \
  //    b---d      plane = a-b-c
  //   /  
  //  c
  */
  template<bool gradients>
  double OBForceFieldUFF::E_OOP() 
  {
    vector<OBFFOOPCalculationUFF>::iterator i;
    double energy = 0.0;
    
    IF_LOGLVL_HIGH {
      Log("\nO U T - O F - P L A N E   B E N D I N G\n\n");
      Log("ATOM TYPES                 OOP     FORCE \n");
      Log(" I    J     K     L       ANGLE   CONSTANT     ENERGY\n");
      Log("----------------------------------------------------------\n");
    }

    unsigned int idxA, idxB, idxC, idxD;
    double angle, e;
    Eigen::Vector3d Fa, Fb, Fc, Fd;
    for (i = _oopcalculations.begin(); i != _oopcalculations.end(); ++i) {
      idxA = i->a->GetIdx() - 1;
      idxB = i->b->GetIdx() - 1;
      idxC = i->c->GetIdx() - 1;
      idxD = i->d->GetIdx() - 1;
 
      if (gradients) {
        angle = VectorOOPDerivative(GetPositions()[idxA], GetPositions()[idxB], GetPositions()[idxC], 
            GetPositions()[idxD], Fa, Fb, Fc, Fd) * DEG_TO_RAD;  
        if (!isfinite(angle))
          angle = 0.0; // doesn't explain why GetAngle is returning NaN but solves it for us;

        // somehow we already get the -1 from the OOPDeriv -- so we'll omit it here
        const double dE = i->koop * (i->c1 * sin(angle) + 2.0 * i->c2 * sin(2.0*angle));
        Fa *= dE;
        Fb *= dE;
        Fc *= dE;
        Fd *= dE;
        GetGradients()[idxA] += Fa;
        GetGradients()[idxB] += Fb;
        GetGradients()[idxC] += Fc;
        GetGradients()[idxD] += Fd;
      } else {
        angle = DEG_TO_RAD * VectorOOP(GetPositions()[idxA], GetPositions()[idxB], 
            GetPositions()[idxC], GetPositions()[idxD]); 
        if (!isfinite(angle))
          angle = 0.0; // doesn't explain why GetAngle is returning NaN but solves it for us;
      }
      
      e = i->koop * (i->c0 + i->c1 * cos(angle) + i->c2 * cos(2.0*angle));
      energy += e;
      
      IF_LOGLVL_HIGH {
         snprintf(_logbuf, BUFF_SIZE, "%-5s %-5s %-5s %-5s%8.3f   %8.3f     %8.3f\n", 
             (*i).a->GetType(), (*i).b->GetType(), (*i).c->GetType(), (*i).d->GetType(), 
             angle * RAD_TO_DEG, (*i).koop, e);
         Log(_logbuf);
       }
     }

     IF_LOGLVL_HIGH {
       snprintf(_logbuf, BUFF_SIZE, "     TOTAL OUT-OF-PLANE BENDING ENERGY = %8.3f\n", energy);
       Log(_logbuf);
     }
     return energy;
  }

  template<bool gradients>
  double OBForceFieldUFF::E_VDW()
  {
    vector<OBFFVDWCalculationUFF>::iterator i;
    double energy = 0.0;
     
    IF_LOGLVL_HIGH {
      Log("\nV A N   D E R   W A A L S\n\n");
      Log("ATOM TYPES\n");
      Log(" I    J        Rij       kij       ENERGY\n");
      Log("-----------------------------------------\n");
      //          XX   XX     -000.000  -000.000  -000.000  -000.000
    }
    
    unsigned int idxA, idxB;
    double rab, e;
    Eigen::Vector3d Fa, Fb;
    unsigned int j = 0;
    for (i = _vdwcalculations.begin(); i != _vdwcalculations.end(); ++i, ++j) {
      // Cut-off check
      if (IsCutOffEnabled())
        if (!GetVDWPairs().BitIsSet(j)) 
          continue;
      
      idxA = i->a->GetIdx() - 1;
      idxB = i->b->GetIdx() - 1;
 
      if (gradients) {
        rab = VectorDistanceDerivative(GetPositions()[idxA], GetPositions()[idxB], Fa, Fb);
      } else {
        const Eigen::Vector3d ab = GetPositions()[idxA] - GetPositions()[idxB];
        rab = ab.norm();
      }
    
      if (IsNearZero(rab, 1.0e-3))
        rab = 1.0e-3;
    
      double term6;
      const double term = i->ka / rab;
      term6 = term * term * term; // ^3
      term6 = term6 * term6; // ^6
      const double term12 = term6 * term6; // ^12
   
      e = i->kab * ((term12) - (2.0 * term6));
    
      if (gradients) { 
        const double term13 = term * term12; // ^13
        const double term7 = term * term6; // ^7
        const double dE = i->kab * 12.0 * (term7 / i->ka - term13 / i->ka);
        Fa *= dE;
        Fb *= dE;
        GetGradients()[idxA] += Fa;
        GetGradients()[idxB] += Fb;
      } 
     
      energy += e;
      
      IF_LOGLVL_HIGH {
        snprintf(_logbuf, BUFF_SIZE, "%-5s %-5s %8.3f  %8.3f  %8.3f\n", (*i).a->GetType(), (*i).b->GetType(), 
                rab, (*i).kab, e);
        Log(_logbuf);
      }
    }

    IF_LOGLVL_MEDIUM {
      snprintf(_logbuf, BUFF_SIZE, "     TOTAL VAN DER WAALS ENERGY = %8.3f %s\n", energy, GetUnit().c_str());
      Log(_logbuf);
    }

    return energy;
  }

  template<bool gradients>
  double OBForceFieldUFF::E_Electrostatic()
  {
    vector<OBFFElectrostaticCalculationUFF>::iterator i;
    double energy = 0.0;
     
    IF_LOGLVL_HIGH {
      Log("\nE L E C T R O S T A T I C   I N T E R A C T I O N S\n\n");
      Log("ATOM TYPES\n");
      Log(" I    J           Rij   332.17*QiQj  ENERGY\n");
      Log("-------------------------------------------\n");
      //            XX   XX     -000.000  -000.000  -000.000  
    }

    unsigned int idxA, idxB;
    double rab, e;
    Eigen::Vector3d Fa, Fb;
    unsigned int j = 0;
    for (i = _electrostaticcalculations.begin(); i != _electrostaticcalculations.end(); ++i, ++j) {
      // Cut-off check
      if (IsCutOffEnabled())
        if (!GetElePairs().BitIsSet(j)) 
          continue;
 
      idxA = i->a->GetIdx() - 1;
      idxB = i->b->GetIdx() - 1;
 
      if (gradients) {
        rab = VectorDistanceDerivative(GetPositions()[idxA], GetPositions()[idxB], Fa, Fb);
      } else {
        const Eigen::Vector3d ab = GetPositions()[idxA] - GetPositions()[idxB];
        rab = ab.norm();
      }
    
      if (IsNearZero(rab, 1.0e-3))
        rab = 1.0e-3;

      e = i->qq / rab;

      if (gradients) {
        const double rab2 = rab * rab;
        const double dE = - (i->qq / rab2);
        Fa *= dE;
        Fb *= dE;
        GetGradients()[idxA] += Fa;
        GetGradients()[idxB] += Fb;
      } 
      
      energy += e;
      
      IF_LOGLVL_HIGH {
        snprintf(_logbuf, BUFF_SIZE, "%-5s %-5s   %8.3f  %8.3f  %8.3f\n", (*i).a->GetType(), (*i).b->GetType(), 
                rab, (*i).qq, e);
        Log(_logbuf);
      }
    }

    IF_LOGLVL_MEDIUM {
      snprintf(_logbuf, BUFF_SIZE, "     TOTAL ELECTROSTATIC ENERGY = %8.3f %s\n", energy, GetUnit().c_str());
      Log(_logbuf);
    }

    return energy;
  }

  //***********************************************
  //Make a global instance
  OBForceFieldUFF theForceFieldUFF("UFF", true);
  //***********************************************

  OBForceFieldUFF::~OBForceFieldUFF()
  {
  }

  double CalculateBondDistance(OBFFParameter *i, OBFFParameter *j, double bondorder)
  {
    double ri, rj;
    double chiI, chiJ;
    double rbo, ren;
    ri = i->_dpar[0];
    rj = j->_dpar[0];
    chiI = i->_dpar[8];
    chiJ = j->_dpar[8];

    // precompute the equilibrium geometry
    // From equation 3
    rbo = -0.1332*(ri+rj)*log(bondorder);
    // From equation 4
    ren = ri*rj*(pow((sqrt(chiI) - sqrt(chiJ)),2.0)) / (chiI*ri + chiJ*rj);
    // From equation 2
    // NOTE: See http://towhee.sourceforge.net/forcefields/uff.html
    // There is a typo in the published paper
    return(ri + rj + rbo - ren);
  }

  bool OBForceFieldUFF::SetupCalculations()
  {
    OBFFParameter *parameterA, *parameterB, *parameterC;
    OBAtom *a, *b, *c, *d;
    
    IF_LOGLVL_LOW
      Log("\nS E T T I N G   U P   C A L C U L A T I O N S\n\n");

    OBMol *mol = GetMolecule();
    vector<OBBitVec> intraGroup = GetIntraGroup();
    vector<OBBitVec> interGroup = GetInterGroup();
    vector<pair<OBBitVec, OBBitVec> > interGroups = GetInterGroups();
 
    // 
    // Bond Calculations
    IF_LOGLVL_LOW
      Log("SETTING UP BOND CALCULATIONS...\n");
    
    OBFFBondCalculationUFF bondcalc;
    double bondorder;

    _bondcalculations.clear();
    
    FOR_BONDS_OF_MOL(bond, mol) {
      a = bond->GetBeginAtom();
      b = bond->GetEndAtom();

      // if there are any groups specified, check if the two bond atoms are in a single intraGroup
      if (HasGroups()) {
        bool validBond = false;
        for (unsigned int i=0; i < intraGroup.size(); ++i) {
          if (intraGroup[i].BitIsOn(a->GetIdx()) && intraGroup[i].BitIsOn(b->GetIdx()))
            validBond = true;
        }
        if (!validBond)
          continue;
      }
 
      bondorder = bond->GetBondOrder(); 
      if (bond->IsAromatic())
        bondorder = 1.5;
      if (bond->IsAmide())
        bondorder = 1.41;
      
      bondcalc.a = a;
      bondcalc.b = b;
      bondcalc.bt = bondorder;

      parameterA = GetParameterUFF(a->GetType(), _ffparams);
      parameterB = GetParameterUFF(b->GetType(), _ffparams);
	
      bondcalc.r0 = CalculateBondDistance(parameterA, parameterB, bondorder);

      // here we fold the 1/2 into the kij from equation 1a
      // Otherwise, this is equation 6 from the UFF paper.
      bondcalc.kb = (0.5 * KCAL_TO_KJ * 664.12 
        * parameterA->_dpar[5] * parameterB->_dpar[5])
        / (bondcalc.r0 * bondcalc.r0 * bondcalc.r0);

      _bondcalculations.push_back(bondcalc);
    }

    //
    // Angle Calculations
    //
    IF_LOGLVL_LOW
      Log("SETTING UP ANGLE CALCULATIONS...\n");
 
    OBFFAngleCalculationUFF anglecalc;
 
    _anglecalculations.clear();
    
    double sinT0;
    double rab, rbc, rac;
    OBBond *bondPtr;
    FOR_ANGLES_OF_MOL(angle, mol) {
      b = mol->GetAtom((*angle)[0] + 1);
      a = mol->GetAtom((*angle)[1] + 1);
      c = mol->GetAtom((*angle)[2] + 1);
      
      // if there are any groups specified, check if the three angle atoms are in a single intraGroup
      if (HasGroups()) {
        bool validAngle = false;
        for (unsigned int i=0; i < intraGroup.size(); ++i) {
          if (intraGroup[i].BitIsOn(a->GetIdx()) && intraGroup[i].BitIsOn(b->GetIdx()) && 
              intraGroup[i].BitIsOn(c->GetIdx()))
            validAngle = true;
        }
        if (!validAngle)
          continue;
      }
 
      anglecalc.a = a;
      anglecalc.b = b;
      anglecalc.c = c;

      parameterA = GetParameterUFF(a->GetType(), _ffparams);
      parameterB = GetParameterUFF(b->GetType(), _ffparams);
      parameterC = GetParameterUFF(c->GetType(), _ffparams);

      anglecalc.coord = parameterB->_ipar[0]; // coordination of central atom

      anglecalc.zi = parameterA->_dpar[5];
      anglecalc.zk = parameterC->_dpar[5];
      anglecalc.theta0 = parameterB->_dpar[1];
      anglecalc.cosT0 = cos(anglecalc.theta0 * DEG_TO_RAD);
      sinT0 = sin(anglecalc.theta0 * DEG_TO_RAD);
      anglecalc.c2 = 1.0 / (4.0 * sinT0 * sinT0);
      anglecalc.c1 = -4.0 * anglecalc.c2 * anglecalc.cosT0;
      anglecalc.c0 = anglecalc.c2*(2.0*anglecalc.cosT0*anglecalc.cosT0 + 1.0);

      // Precompute the force constant
      bondPtr = mol->GetBond(a,b);
      bondorder = bondPtr->GetBondOrder(); 
      if (bondPtr->IsAromatic())
        bondorder = 1.5;
      if (bondPtr->IsAmide())
        bondorder = 1.41;      
      rab = CalculateBondDistance(parameterA, parameterB, bondorder);

      bondPtr = mol->GetBond(b,c);
      bondorder = bondPtr->GetBondOrder(); 
      if (bondPtr->IsAromatic())
        bondorder = 1.5;
      if (bondPtr->IsAmide())
        bondorder = 1.41;
      rbc = CalculateBondDistance(parameterB, parameterC, bondorder);
      rac = sqrt(rab*rab + rbc*rbc - 2.0 * rab*rbc*anglecalc.cosT0);

      // Equation 13 from paper -- corrected by Towhee
      // Note that 1/(rij * rjk) cancels with rij*rjk in eqn. 13
      anglecalc.ka = (644.12 * KCAL_TO_KJ) * (anglecalc.zi * anglecalc.zk / (pow(rac, 5.0)));
      anglecalc.ka *= (3.0*rab*rbc*(1.0 - anglecalc.cosT0*anglecalc.cosT0) - rac*rac*anglecalc.cosT0);
     
      _anglecalculations.push_back(anglecalc);
    }
    
    //
    // Torsion Calculations
    //
    IF_LOGLVL_LOW
      Log("SETTING UP TORSION CALCULATIONS...\n");
 
    OBFFTorsionCalculationUFF torsioncalc;
    double torsiontype;
    double phi0 = 0.0;

    _torsioncalculations.clear();
 
    double vi, vj;
    FOR_TORSIONS_OF_MOL(t, mol) {
      a = mol->GetAtom((*t)[0] + 1);
      b = mol->GetAtom((*t)[1] + 1);
      c = mol->GetAtom((*t)[2] + 1);
      d = mol->GetAtom((*t)[3] + 1);

      // if there are any groups specified, check if the four torsion atoms are in a single intraGroup
      if (HasGroups()) {
        bool validTorsion = false;
        for (unsigned int i=0; i < intraGroup.size(); ++i) {
          if (intraGroup[i].BitIsOn(a->GetIdx()) && intraGroup[i].BitIsOn(b->GetIdx()) && 
              intraGroup[i].BitIsOn(c->GetIdx()) && intraGroup[i].BitIsOn(d->GetIdx()))
            validTorsion = true;
        }
        if (!validTorsion)
          continue;
      }
 
      OBBond *bc = mol->GetBond(b, c);
      torsiontype = bc->GetBondOrder(); 
      if (bc->IsAromatic())
        torsiontype = 1.5;
      if (bc->IsAmide())
        torsiontype = 1.41;
      
      torsioncalc.a = a;
      torsioncalc.b = b;
      torsioncalc.c = c;
      torsioncalc.d = d;
      torsioncalc.tt = torsiontype;

      parameterB = GetParameterUFF(b->GetType(), _ffparams);
      parameterC = GetParameterUFF(c->GetType(), _ffparams);

      if (parameterB->_ipar[0] == 3 && parameterC->_ipar[0] == 3) {
        // two sp3 centers
        phi0 = 60.0;
        torsioncalc.n = 3;
        vi = parameterB->_dpar[6];
        vj = parameterC->_dpar[6];

        // exception for a pair of group 6 sp3 atoms
        switch (b->GetAtomicNum()) {
        case 8:
          vi = 2.0;
          torsioncalc.n = 2;
          phi0 = 90.0;
          break;
        case 16:
        case 34:
        case 52:
        case 84:
          vi = 6.8;
          torsioncalc.n = 2;
          phi0 = 90.0;
        }
        switch (c->GetAtomicNum()) {
        case 8:
          vj = 2.0;
          torsioncalc.n = 2;
          phi0 = 90.0;
          break;
        case 16:
        case 34:
        case 52:
        case 84:
          vj = 6.8;
          torsioncalc.n = 2;
          phi0 = 90.0;
        }

        torsioncalc.V = 0.5 * KCAL_TO_KJ * sqrt(vi * vj);

      } else if (parameterB->_ipar[0] == 2 && parameterC->_ipar[0] == 2) {
        // two sp2 centers
        phi0 = 180.0;
        torsioncalc.n = 2;
        torsioncalc.V = 0.5 * KCAL_TO_KJ * 5.0 *
          sqrt(parameterB->_dpar[7]*parameterC->_dpar[7]) * 
          (1.0 + 4.18 * log(torsiontype));
      } else if ((parameterB->_ipar[0] == 2 && parameterC->_ipar[0] == 3)
                 || (parameterB->_ipar[0] == 3 && parameterC->_ipar[0] == 2)) {
        // one sp3, one sp2
        phi0 = 0.0;
        torsioncalc.n = 6;
        torsioncalc.V = 0.5 * KCAL_TO_KJ * 1.0;

        // exception for group 6 sp3
        if (parameterC->_ipar[0] == 3) {
          switch (c->GetAtomicNum()) {
          case 8:
          case 16:
          case 34:
          case 52:
          case 84:
            torsioncalc.n = 2;
            phi0 = 90.0;
          }
        }
        if (parameterB->_ipar[0] == 3) {
          switch (b->GetAtomicNum()) {
          case 8:
          case 16:
          case 34:
          case 52:
          case 84:
            torsioncalc.n = 2;
            phi0 = 90.0;
          }
        }
      }

      if (IsNearZero(torsioncalc.V)) // don't bother calcuating this torsion
        continue;

      // still need to implement special case of sp2-sp3 with sp2-sp2

      torsioncalc.cosNPhi0 = cos(torsioncalc.n * DEG_TO_RAD * phi0);
      _torsioncalculations.push_back(torsioncalc);     
    }
    
    //
    // OOP/Inversion Calculations
    //
    IF_LOGLVL_LOW
      Log("SETTING UP OOP CALCULATIONS...\n");
    OBFFOOPCalculationUFF oopcalc;

    _oopcalculations.clear();
 
    double phi;
    // The original Rappe paper in JACS isn't very clear about the parameters
    // The following was adapted from Towhee
    FOR_ATOMS_OF_MOL(atom, mol) {
      b = (OBAtom*) &*atom;

      switch (b->GetAtomicNum()) {
      case 6: // carbon
      case 7: // nitrogen
      case 8: // oxygen
      case 15: // phos.
      case 33: // as
      case 51: // sb
      case 83: // bi
        break;
      default: // no inversion term for this element
        continue;
      }

      a = NULL;
      c = NULL;
      d = NULL;
      
      if (EQn(b->GetType(), "N_3", 3) ||
          EQn(b->GetType(), "N_2", 3) ||
          EQn(b->GetType(), "N_R", 3) ||
          EQn(b->GetType(), "O_2", 3) ||
          EQn(b->GetType(), "O_R", 3)) {
        oopcalc.c0 = 1.0;
        oopcalc.c1 = -1.0;
        oopcalc.c2 = 0.0;
        oopcalc.koop = 6.0 * KCAL_TO_KJ;
      }
      else if (EQn(b->GetType(), "P_3+3", 5) ||
               EQn(b->GetType(), "As3+3", 5) ||
               EQn(b->GetType(), "Sb3+3", 5) ||
               EQn(b->GetType(), "Bi3+3", 5)) {

        if (EQn(b->GetType(), "P_3+3", 5))
          phi = 84.4339 * DEG_TO_RAD;
        else if (EQn(b->GetType(), "As3+3", 5))
          phi = 86.9735 * DEG_TO_RAD;
        else if (EQn(b->GetType(), "Sb3+3", 5))
          phi = 87.7047 * DEG_TO_RAD;
        else
          phi = 90.0 * DEG_TO_RAD;

        oopcalc.c1 = -4.0 * cos(phi);
        oopcalc.c2 = 1.0;
        oopcalc.c0 = -1.0*oopcalc.c1 * cos(phi) + oopcalc.c2*cos(2.0*phi);
        oopcalc.koop = 22.0 * KCAL_TO_KJ;
      }
      else if (!(EQn(b->GetType(), "C_2", 3) || EQn(b->GetType(), "C_R", 3)))
        continue; // inversion not defined for this atom type

      // C atoms, we should check if we're bonded to O
      FOR_NBORS_OF_ATOM(nbr, b) {
        if (a == NULL)
          a = (OBAtom*) &*nbr;
        else if (c == NULL)
          c = (OBAtom*) &*nbr;
        else
          d = (OBAtom*) &*nbr;
      }
	  
      if ((a == NULL) || (c == NULL) || (d == NULL))
        continue;
 
      // if there are any groups specified, check if the four oop atoms are in a single intraGroup
      if (HasGroups()) {
        bool validOOP = false;
        for (unsigned int i=0; i < intraGroup.size(); ++i) {
          if (intraGroup[i].BitIsOn(a->GetIdx()) && intraGroup[i].BitIsOn(b->GetIdx()) && 
              intraGroup[i].BitIsOn(c->GetIdx()) && intraGroup[i].BitIsOn(d->GetIdx()))
            validOOP = true;
        }
        if (!validOOP)
          continue;
      }
 
      if (EQn(b->GetType(), "C_2", 3) || EQn(b->GetType(), "C_R", 3)) {
        oopcalc.c0 = 1.0;
        oopcalc.c1 = -1.0;
        oopcalc.c2 = 0.0;
        oopcalc.koop = 6.0 * KCAL_TO_KJ;
        if (EQn(a->GetType(), "O_2", 3) ||
            EQn(c->GetType(), "O_2", 3) ||
            EQn(d->GetType(), "O_2", 3)) {
          oopcalc.koop = 50.0 * KCAL_TO_KJ;
        }
      }
            
      // A-B-CD || C-B-AD  PLANE = ABC
      oopcalc.a = a;
      oopcalc.b = b;
      oopcalc.c = c;
      oopcalc.d = d;
      oopcalc.koop /= 3.0; // three OOPs to consider
	    
      _oopcalculations.push_back(oopcalc);
      
      // C-B-DA || D-B-CA  PLANE BCD
      oopcalc.a = d;
      oopcalc.d = a;
      
      _oopcalculations.push_back(oopcalc);
      
      // A-B-DC || D-B-AC  PLANE ABD
      oopcalc.a = a;
      oopcalc.c = d;
      oopcalc.d = c;
	    
      _oopcalculations.push_back(oopcalc);
    } // for all atoms

    // 
    // VDW Calculations
    //
    IF_LOGLVL_LOW
      Log("SETTING UP VAN DER WAALS CALCULATIONS...\n");
    
    OBFFVDWCalculationUFF vdwcalc;

    _vdwcalculations.clear();
    
    FOR_PAIRS_OF_MOL(p, mol) {
      a = mol->GetAtom((*p)[0]);
      b = mol->GetAtom((*p)[1]);

      // if there are any groups specified, check if the two atoms are in a single interGroup or if
      // two two atoms are in one of the interGroups pairs.
      if (HasGroups()) {
        bool validVDW = false;
        for (unsigned int i=0; i < interGroup.size(); ++i) {
          if (interGroup[i].BitIsOn(a->GetIdx()) && interGroup[i].BitIsOn(b->GetIdx())) 
            validVDW = true;
        }
        for (unsigned int i=0; i < interGroups.size(); ++i) {
          if (interGroups[i].first.BitIsOn(a->GetIdx()) && interGroups[i].second.BitIsOn(b->GetIdx())) 
            validVDW = true;
          if (interGroups[i].first.BitIsOn(b->GetIdx()) && interGroups[i].second.BitIsOn(a->GetIdx())) 
            validVDW = true;
        }
 
        if (!validVDW)
          continue;
      }
 
      if (a->IsConnected(b)) {
        continue;
      }
      if (a->IsOneThree(b)) {
        continue;
      }

      parameterA = GetParameterUFF(a->GetType(), _ffparams);
      parameterB = GetParameterUFF(b->GetType(), _ffparams);

      vdwcalc.Ra = parameterA->_dpar[2];
      vdwcalc.ka = parameterA->_dpar[3];
      vdwcalc.Rb = parameterB->_dpar[2];
      vdwcalc.kb = parameterB->_dpar[3];

      vdwcalc.a = &*a;
      vdwcalc.b = &*b;
     
      //this calculations only need to be done once for each pair, 
      //we do them now and save them for later use
      vdwcalc.kab = KCAL_TO_KJ * sqrt(vdwcalc.ka * vdwcalc.kb);
      
      // 1-4 scaling
      // This isn't mentioned in the UFF paper, but is common for other methods
      //       if (a->IsOneFour(b))
      //         vdwcalc.kab *= 0.5;

      // ka now represents the xij in equation 20 -- the expected vdw distance
      vdwcalc.ka = sqrt(vdwcalc.Ra * vdwcalc.Rb);
      
      _vdwcalculations.push_back(vdwcalc);
    }
    
    // NOTE: No electrostatics are set up
    // If you want electrostatics with UFF (not a good idea), you will need to call SetupElectrostatics
    
    return true;
  }

  bool OBForceFieldUFF::SetupElectrostatics()
  {
    OBMol *mol = GetMolecule();
    vector<OBBitVec> interGroup = GetInterGroup();
    vector<pair<OBBitVec, OBBitVec> > interGroups = GetInterGroups();
    // 
    // Electrostatic Calculations
    //
    OBAtom *a, *b;

    IF_LOGLVL_LOW
      Log("SETTING UP ELECTROSTATIC CALCULATIONS...\n");
 
    OBFFElectrostaticCalculationUFF elecalc;

    _electrostaticcalculations.clear();
    
    // Note that while the UFF paper mentions an electrostatic term,
    // it does not actually use it. Both Towhee and the UFF FAQ
    // discourage the use of electrostatics with UFF.

    FOR_PAIRS_OF_MOL(p, mol) {
      a = mol->GetAtom((*p)[0]);
      b = mol->GetAtom((*p)[1]);
      
      // if there are any groups specified, check if the two atoms are in a single interGroup or if
      // two two atoms are in one of the interGroups pairs.
      if (HasGroups()) {
        bool validEle = false;
        for (unsigned int i=0; i < interGroup.size(); ++i) {
          if (interGroup[i].BitIsOn(a->GetIdx()) && interGroup[i].BitIsOn(b->GetIdx())) 
            validEle = true;
        }
        for (unsigned int i=0; i < interGroups.size(); ++i) {
          if (interGroups[i].first.BitIsOn(a->GetIdx()) && interGroups[i].second.BitIsOn(b->GetIdx())) 
            validEle = true;
          if (interGroups[i].first.BitIsOn(b->GetIdx()) && interGroups[i].second.BitIsOn(a->GetIdx())) 
            validEle = true;
        }
 
        if (!validEle)
          continue;
      }
 
      if (a->IsConnected(b)) {
        continue;
      }
      if (a->IsOneThree(b)) {
        continue;
      }
      
      // Remember that at the moment, this term is not currently used
      // These are also the Gasteiger charges, not the Qeq mentioned in the UFF paper
      elecalc.qq = KCAL_TO_KJ * 332.0637 * a->GetPartialCharge() * b->GetPartialCharge();
      
      if (elecalc.qq) {
        elecalc.a = &*a;
        elecalc.b = &*b;
        
        _electrostaticcalculations.push_back(elecalc);
      }
    }
    return true;
  }

  bool OBForceFieldUFF::ParseParamFile()
  {
    vector<string> vs;
    char buffer[BUFF_SIZE];
    
    OBFFParameter parameter;

    // open data/UFF.prm
    ifstream ifs;
    if (OpenDatafile(ifs, "UFF.prm").length() == 0) {
      obErrorLog.ThrowError(__FUNCTION__, "Cannot open UFF.prm", obError);
      return false;
    }

    // Set the locale for number parsing to avoid locale issues: PR#1785463
    obLocale.SetLocale();

    while (ifs.getline(buffer, BUFF_SIZE)) {
      tokenize(vs, buffer);
      if (vs.size() < 13)
        continue;
      
      if (EQn(buffer, "param", 5)) {
        // set up all parameters from this
        parameter.clear();
        parameter._a = vs[1]; // atom type
        parameter._dpar.push_back(atof(vs[2].c_str())); // r1
        parameter._dpar.push_back(atof(vs[3].c_str())); // theta0
        parameter._dpar.push_back(atof(vs[4].c_str())); // x1
        parameter._dpar.push_back(atof(vs[5].c_str())); // D1
        parameter._dpar.push_back(atof(vs[6].c_str())); // zeta
        parameter._dpar.push_back(atof(vs[7].c_str())); // Z1
        parameter._dpar.push_back(atof(vs[8].c_str())); // Vi
        parameter._dpar.push_back(atof(vs[9].c_str())); // Uj
        parameter._dpar.push_back(atof(vs[10].c_str())); // Xi
        parameter._dpar.push_back(atof(vs[11].c_str())); // Hard
        parameter._dpar.push_back(atof(vs[12].c_str())); // Radius

        char coord = vs[1][2]; // 3rd character of atom type
        switch (coord) {
        case '1': // linear
          parameter._ipar.push_back(1);
          break;
        case '2': // trigonal planar (sp2)
        case 'R': // aromatic (N_R)
          parameter._ipar.push_back(2);
          break;
        case '3': // tetrahedral (sp3)
          parameter._ipar.push_back(3);
          break;
        case '4': // square planar
          parameter._ipar.push_back(4);
          break;
        case '5': // trigonal bipyramidal -- not actually in parameterization
          parameter._ipar.push_back(5);
          break;
        case '6': // octahedral
          parameter._ipar.push_back(6);
          break;
        case '7': // pentagonal bipyramidal -- not actually in parameterization
            parameter._ipar.push_back(7);
            break;
        default: // general case (unknown coordination)
          // These atoms appear to generally be linear coordination like Cl
          parameter._ipar.push_back(1);
        }
        
        _ffparams.push_back(parameter);
      }
    }
	
    if (ifs)
      ifs.close();
 
    // return the locale to the original one
    obLocale.RestoreLocale();
 
    return 0;
  }
  
  bool OBForceFieldUFF::SetTypes()
  {
    OBMol *mol = GetMolecule();
    vector<vector<int> > _mlist; //!< match list for atom typing
    vector<pair<OBSmartsPattern*,string> > _vexttyp; //!< external atom type rules
    vector<vector<int> >::iterator j;
    vector<pair<OBSmartsPattern*,string> >::iterator i;
    OBSmartsPattern *sp;
    vector<string> vs;
    char buffer[BUFF_SIZE];
 
    mol->SetAtomTypesPerceived();
    
    // open data/UFF.prm
    ifstream ifs;
    if (OpenDatafile(ifs, "UFF.prm").length() == 0) {
      obErrorLog.ThrowError(__FUNCTION__, "Cannot open UFF.prm", obError);
      return false;
    }

    while (ifs.getline(buffer, BUFF_SIZE)) {
      if (EQn(buffer, "atom", 4)) {
      	tokenize(vs, buffer);

        sp = new OBSmartsPattern;
        if (sp->Init(vs[1])) {
          _vexttyp.push_back(pair<OBSmartsPattern*,string> (sp,vs[2]));
        }
        else {
          delete sp;
          sp = NULL;
          obErrorLog.ThrowError(__FUNCTION__, " Could not parse atom type table from UFF.prm", obInfo);
          return false;
        }    
      }
    }
 
    for (i = _vexttyp.begin();i != _vexttyp.end();++i) {
      if (i->first->Match(*mol)) {
        _mlist = i->first->GetMapList();
        for (j = _mlist.begin();j != _mlist.end();++j) {
          mol->GetAtom((*j)[0])->SetType(i->second);
        }
      }
    }
    IF_LOGLVL_LOW {
      Log("\nA T O M   T Y P E S\n\n");
      Log("IDX\tTYPE\n");
      
      FOR_ATOMS_OF_MOL (a, mol) {
        snprintf(_logbuf, BUFF_SIZE, "%d\t%s\n", a->GetIdx(), a->GetType());
        Log(_logbuf);
      }

    }
     
    if (ifs)
      ifs.close();

    return true;
  }
  
  double OBForceFieldUFF::Energy(bool gradients)
  {
    double energy = 0.0;
    
    IF_LOGLVL_MEDIUM
      Log("\nE N E R G Y\n\n");
    
    if (gradients) {
      ClearGradients();
      if (m_terms & BondTerm)
        energy  = E_Bond<true>();
      if (m_terms & AngleTerm)
        energy += E_Angle<true>();
      if (m_terms & TorsionTerm)
        energy += E_Torsion<true>();
      if (m_terms & OOPTerm)
        energy += E_OOP<true>();
      if (m_terms & VDWTerm)
        energy += E_VDW<true>();
    } else {
      if (m_terms & BondTerm)
        energy  = E_Bond<false>();
      if (m_terms & AngleTerm)
        energy += E_Angle<false>();
      if (m_terms & TorsionTerm)
        energy += E_Torsion<false>();
      if (m_terms & OOPTerm)
        energy += E_OOP<false>();
      if (m_terms & VDWTerm)
        energy += E_VDW<false>();
    }

    // The electrostatic term, by default is 0.0
    // You will need to call SetupEletrostatics if you want it
    // energy += E_Electrostatic(gradients);
     
    IF_LOGLVL_MEDIUM {
      snprintf(_logbuf, BUFF_SIZE, "\nTOTAL ENERGY = %8.5f %s\n", energy, GetUnit().c_str());
      Log(_logbuf);
    }

    return energy;
  }
  
  OBFFParameter* OBForceFieldUFF::GetParameterUFF(std::string a, vector<OBFFParameter> &parameter)
  {
    for (unsigned int idx=0; idx < parameter.size(); ++idx) {
      if (a == parameter[idx]._a) {
        return &parameter[idx];
      }
    }
    return NULL;
  }
 
  bool OBForceFieldUFF::ValidateGradients ()
  {
    OBMol *mol = GetMolecule();
    Eigen::Vector3d numgrad, anagrad, err;
    bool passed = true; // set to false if any component fails
    int idx;
    
    Log("\nV A L I D A T E   G R A D I E N T S\n\n");
    Log("ATOM IDX      NUMERICAL GRADIENT           ANALYTICAL GRADIENT        REL. ERROR (%)   \n");
    Log("----------------------------------------------------------------------------------------\n");
    //     "XX       (000.000, 000.000, 000.000)  (000.000, 000.000, 000.000)  (00.00, 00.00, 00.00)"
  
    FOR_ATOMS_OF_MOL (a, mol) {
      idx = (a->GetIdx() - 1);
      
      // OBFF_ENERGY (i.e., overall)
      numgrad = NumericalDerivative(idx);
      Energy(); // compute
      anagrad = GetGradients()[idx];
      err = ValidateGradientError(numgrad, anagrad);

      snprintf(_logbuf, BUFF_SIZE, "%2d       (%7.3f, %7.3f, %7.3f)  (%7.3f, %7.3f, %7.3f)  (%5.2f, %5.2f, %5.2f)\n", a->GetIdx(), numgrad.x(), numgrad.y(), numgrad.z(), 
              anagrad.x(), anagrad.y(), anagrad.z(), err.x(), err.y(), err.z());
      Log(_logbuf);
      
      SetAllTermsEnabled(false);
      
      // OBFF_EBOND
      SetTermEnabled(BondTerm, true);
      numgrad = NumericalDerivative(idx);
      SetTermEnabled(BondTerm, false);
      ClearGradients();
      E_Bond(); // compute
      anagrad = GetGradients()[idx];
      err = ValidateGradientError(numgrad, anagrad);

      snprintf(_logbuf, BUFF_SIZE, "    bond    (%7.3f, %7.3f, %7.3f)  (%7.3f, %7.3f, %7.3f)  (%5.2f, %5.2f, %5.2f)\n", numgrad.x(), numgrad.y(), numgrad.z(), 
              anagrad.x(), anagrad.y(), anagrad.z(), err.x(), err.y(), err.z());
      Log(_logbuf);
      if (err.x() > 5.0 || err.y() > 5.0 || err.z() > 5.0)
        passed = false;
      
      // OBFF_EANGLE
      SetTermEnabled(AngleTerm, true);
      numgrad = NumericalDerivative(idx);
      SetTermEnabled(AngleTerm, false);
      ClearGradients();
      E_Angle(); // compute
      anagrad = GetGradients()[idx];
      err = ValidateGradientError(numgrad, anagrad);

      snprintf(_logbuf, BUFF_SIZE, "    angle   (%7.3f, %7.3f, %7.3f)  (%7.3f, %7.3f, %7.3f)  (%5.2f, %5.2f, %5.2f)\n", numgrad.x(), numgrad.y(), numgrad.z(), 
              anagrad.x(), anagrad.y(), anagrad.z(), err.x(), err.y(), err.z());
      Log(_logbuf);
      if (err.x() > 5.0 || err.y() > 5.0 || err.z() > 5.0)
        passed = false;
      
      // OBFF_ETORSION
      SetTermEnabled(TorsionTerm, true);
      numgrad = NumericalDerivative(idx);
      SetTermEnabled(TorsionTerm, false);
      ClearGradients();
      E_Torsion(); // compute
      anagrad = GetGradients()[idx];
      err = ValidateGradientError(numgrad, anagrad);

      snprintf(_logbuf, BUFF_SIZE, "    torsion (%7.3f, %7.3f, %7.3f)  (%7.3f, %7.3f, %7.3f)  (%5.2f, %5.2f, %5.2f)\n", numgrad.x(), numgrad.y(), numgrad.z(), 
              anagrad.x(), anagrad.y(), anagrad.z(), err.x(), err.y(), err.z());
      Log(_logbuf);
      // 8% tolerance here because some 180 torsions cause numerical instability
      if (err.x() > 8.0 || err.y() > 8.0 || err.z() > 8.0)
        passed = false;
      
      // OBFF_EOOP
      SetTermEnabled(OOPTerm, true);
      numgrad = NumericalDerivative(idx);
      SetTermEnabled(OOPTerm, false);
      ClearGradients();
      E_OOP(); // compute
      anagrad = GetGradients()[idx];
      err = ValidateGradientError(numgrad, anagrad);

      snprintf(_logbuf, BUFF_SIZE, "    oop     (%7.3f, %7.3f, %7.3f)  (%7.3f, %7.3f, %7.3f)  (%5.2f, %5.2f, %5.2f)\n", numgrad.x(), numgrad.y(), numgrad.z(), 
              anagrad.x(), anagrad.y(), anagrad.z(), err.x(), err.y(), err.z());
      Log(_logbuf);
//      if (err.x() > 5.0 || err.y() > 5.0 || err.z() > 5.0)
//        passed = false;
        
      // OBFF_EVDW
      SetTermEnabled(VDWTerm, true);
      numgrad = NumericalDerivative(idx);
      SetTermEnabled(VDWTerm, false);
      ClearGradients();
      E_VDW(); // compute
      anagrad = GetGradients()[idx];
      err = ValidateGradientError(numgrad, anagrad);

      snprintf(_logbuf, BUFF_SIZE, "    vdw     (%7.3f, %7.3f, %7.3f)  (%7.3f, %7.3f, %7.3f)  (%5.2f, %5.2f, %5.2f)\n", numgrad.x(), numgrad.y(), numgrad.z(), 
              anagrad.x(), anagrad.y(), anagrad.z(), err.x(), err.y(), err.z());
      Log(_logbuf);
      if (err.x() > 5.0 || err.y() > 5.0 || err.z() > 5.0)
        passed = false;

      /*
      // OBFF_EELECTROSTATIC
      SetTermEnabled(EleTerm, true);
      numgrad = NumericalDerivative(idx);
      SetTermEnabled(EleTerm, false);
      ClearGradients();
      E_Electrostatic(); // compute
      anagrad = GetGradients()[idx];
      err = ValidateGradientError(numgrad, anagrad);

      snprintf(_logbuf, BUFF_SIZE, "    electro (%7.3f, %7.3f, %7.3f)  (%7.3f, %7.3f, %7.3f)  (%5.2f, %5.2f, %5.2f)\n", numgrad.x(), numgrad.y(), numgrad.z(), 
              anagrad.x(), anagrad.y(), anagrad.z(), err.x(), err.y(), err.z());
      Log(_logbuf);
      if (err.x() > 5.0 || err.y() > 5.0 || err.z() > 5.0)
        passed = false;
      */
    
      SetAllTermsEnabled(true);
    }
    
    
    return passed; // did we pass every single component?
  }
  
} // end namespace OpenBabel

//! \file forcefieldUFF.cpp
//! \brief UFF force field
