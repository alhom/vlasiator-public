/*
 * This file is part of Vlasiator.
 * 
 * Copyright 2010, 2011, 2012 Finnish Meteorological Institute
 * 
 * Vlasiator is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3
 * as published by the Free Software Foundation.
 * 
 * Vlasiator is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <cstdlib>
#include <mpi.h>
#include <iostream>
#include <limits>

#include "sysboundarycondition.h"
#include "outflow.h"
#include "../parameters.h"

using namespace std;

namespace SBC {
   Outflow::Outflow(): SysBoundaryCondition() { }
   Outflow::~Outflow() { }
   
   bool Outflow::initSysBoundary() {
      /* The bit field encodes which of the +x, -x, +y, -y, +z, -z faces are to have outflow system boundary conditions.
       * The bit field has length 6, a bit raised to 1 indicates the corresponding face will have outflow.
       * The 6 bits left-to-right correspond to +x, -x, +y, -y, +z, -z respectively.
       */
      faces = 0;
      vector<string>::const_iterator it;
      for (it = Parameters::outflowFaceList.begin();
           it != Parameters::outflowFaceList.end();
      it++) {
         if(*it == "x+") faces = faces|(1<<5);
         if(*it == "x-") faces = faces|(1<<4);
         if(*it == "y+") faces = faces|(1<<3);
         if(*it == "y-") faces = faces|(1<<2);
         if(*it == "z+") faces = faces|(1<<1);
         if(*it == "z-") faces = faces|1;
      }
      return true;
   }
   
   int Outflow::assignSysBoundary(creal* cellParams) {
      creal dx = cellParams[CellParams::DX];
      creal dy = cellParams[CellParams::DY];
      creal dz = cellParams[CellParams::DZ];
      creal x = cellParams[CellParams::XCRD] + 0.5*dx;
      creal y = cellParams[CellParams::YCRD] + 0.5*dy;
      creal z = cellParams[CellParams::ZCRD] + 0.5*dz;
      
      int typeToAssign = sysboundarytype::NOT_SYSBOUNDARY;
      isThisCellOnAFace = 0;
      
      if(x > Parameters::xmax - dx) isThisCellOnAFace = isThisCellOnAFace|(1<<5);
      if(x < Parameters::xmin + dx) isThisCellOnAFace = isThisCellOnAFace|(1<<4);
      if(y > Parameters::ymax - dy) isThisCellOnAFace = isThisCellOnAFace|(1<<3);
      if(y < Parameters::ymin + dy) isThisCellOnAFace = isThisCellOnAFace|(1<<2);
      if(z > Parameters::zmax - dz) isThisCellOnAFace = isThisCellOnAFace|(1<<1);
      if(z < Parameters::zmin + dz) isThisCellOnAFace = isThisCellOnAFace|1;
      
      // Bitwise comparison of the field defining which faces to use and the field telling on which faces this cell is
      if((faces & isThisCellOnAFace) != 0) typeToAssign = getIndex();
      return typeToAssign;
   }
   
   Real Outflow::calcPhaseSpaceDensity(creal& x,creal& y,creal& z,
                              creal& dx,creal& dy,creal& dz,
                              creal& vx,creal& vy,creal& vz,
                              creal& dvx,creal& dvy,creal& dvz
   ) {return 0.0;}
   
   void Outflow::calcCellParameters(Real* cellParams, creal& t) { }
   
   std::string Outflow::getName() const {return "Outflow";}
   
   uint Outflow::getIndex() const {return sysboundarytype::OUTFLOW;}
   uint Outflow::getPrecedence() const {return Parameters::outflowPrecedence;}
}
