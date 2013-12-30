/*
This file is part of Vlasiator.

Copyright 2010, 2011, 2012, 2013 Finnish Meteorological Institute

*/


#include <cstdlib>
#include <iostream>
#include <vector>

#ifdef _OPENMP
#include "omp.h"
#endif
#include <zoltan.h>

#include "../vlasovmover.h"
#include "phiprof.hpp"

#include "cpu_moments.h"
#include "cpu_acc_semilag.hpp"
#include "cpu_trans_leveque.h"



#include <stdint.h>
#include <dccrg.hpp>

#include "../transferstencil.h"
#include "spatial_cell.hpp"
#include "../grid.h"

using namespace std;
using namespace spatial_cell;

//compute one acceleration substep 
void calculateAccelerationSubstep(
   const dccrg::Dccrg<SpatialCell>& mpiGrid,   
   const vector< CellID > &propagatedCells,
   const vector< Real > &subDt);


static TransferStencil<CellID> stencilAverages(INVALID_CELLID);
static TransferStencil<CellID> stencilUpdates(INVALID_CELLID);

//list of local cells that are system boundary cells
//FIXME, it could probably be removed, and just read directly if a cell is  a boundary cell or not
static set<CellID> sysBoundaryCells;


static map<pair<CellID,int>,Real*> updateBuffers;
/**< For each local cell receiving one or more remote df/dt updates,
   * MPI rank of remote process sending an update and address to the 
   * allocated buffer. */
static map<CellID,vector< vector<Real> > > remoteUpdates;
/**< For each local cell receiving one or more remote df/dt updates, 
   * a set containing addresses of all allocated buffers. Note that map 
   * remoteUpdates is only used to iterate over all df/dt buffers, which 
   * is inconvenient to do with updateBuffers. updateBuffers is in convenient 
   * form to post MPI receives, remoteUpdates is convenient to iterate 
   * all cell's remote updates.*/

CellID getNeighbourID(
   const dccrg::Dccrg<SpatialCell>& mpiGrid,
   const CellID& cellID,
   const uchar& i,
   const uchar& j,
   const uchar& k
) {
   // TODO: merge this with the one in lond...anna.cpp
   const std::vector<CellID> neighbors = mpiGrid.get_neighbors_of(cellID, int(i) - 2, int(j) - 2, int(k) - 2);
   if (neighbors.size() == 0) {
       std::cerr << __FILE__ << ":" << __LINE__
                 << " No neighbor for cell " << cellID
                 << " at offsets " << int(i) - 2 << ", " << int(j) - 2 << ", " << int(k) - 2
                 << std::endl;
       abort();
   }
   // TODO support spatial refinement
   if( neighbors[0] == INVALID_CELLID  ||
       mpiGrid[neighbors[0]]->sysBoundaryFlag == sysboundarytype::DO_NOT_COMPUTE ||
       (mpiGrid[neighbors[0]]->sysBoundaryLayer != 1  &&
        mpiGrid[neighbors[0]]->sysBoundaryFlag != sysboundarytype::NOT_SYSBOUNDARY )
      ) {
      return INVALID_CELLID;
   } else {
      return neighbors[0];
   }
}

bool initializeSpatialLeveque(dccrg::Dccrg<SpatialCell>& mpiGrid) { 
   
   // Exchange sysBoundaryFlags between neighbouring processes, 
   // so that boundary condition functions are correctly called 
   // for remote system boundary cells:
   SpatialCell::set_mpi_transfer_type(Transfer::CELL_SYSBOUNDARYFLAG);
   mpiGrid.update_remote_neighbor_data(VLASOV_SOLVER_NEIGHBORHOOD_ID);
   // Populate spatial neighbour list:
   vector<CellID> cells,remoteCells;
   std::vector<CellID> nbrs; //temporary vector for neighbors at certain offset
   
   cells=mpiGrid.get_cells();

   
   for (size_t cell=0; cell<cells.size(); ++cell) {
      cuint cellID = cells[cell];
      SpatialCell *SC = mpiGrid[cellID];

      SC->neighbors.clear();
//      SC->isSysBoundaryCell = false;
      SC->procBoundaryFlag=0;
      // Get spatial neighbour IDs and store them into a vector:
      for (int k=-1; k<2; ++k) for (int j=-1; j<2; ++j) for (int i=-1; i<2; ++i) {
          // in dccrg cells are neighbors of themselves only with periodic boundaries
         if (i == 0 && j == 0 && k == 0) {
            SC->neighbors.push_back(cellID);
         }
         else {
            SC->neighbors.push_back(getNeighbourID(mpiGrid, cellID, 2+i, 2+j, 2+k));
//             if (SC->neighbors.back() == INVALID_CELLID) {
//                //we only use a stencil of one to set if a cell is a system boundary cell.
//                SC->isSysBoundaryCell = true;
//             }
         }
      }

      if (getNeighbourID(mpiGrid, cellID, 0, 2, 2) != INVALID_CELLID) {
         SC->neighbors.push_back(getNeighbourID(mpiGrid, cellID, 0, 2, 2));
      }
      else{
         //outside boundary, attempt to use one closer 
         SC->neighbors.push_back(getNeighbourID(mpiGrid, cellID, 1, 2, 2));
      }

      if (getNeighbourID(mpiGrid, cellID, 2, 0, 2) != INVALID_CELLID) {
         SC->neighbors.push_back(getNeighbourID(mpiGrid, cellID, 2, 0, 2));
      }
      else{
         //outside boundary, attempt to use one closer 
         SC->neighbors.push_back(getNeighbourID(mpiGrid, cellID, 2, 1, 2));
      }

      if (getNeighbourID(mpiGrid, cellID, 2, 2, 0) != INVALID_CELLID) {
         SC->neighbors.push_back(getNeighbourID(mpiGrid, cellID, 2, 2, 0));
      }
      else{
         //outside boundary, attempt to use one closer 
         SC->neighbors.push_back(getNeighbourID(mpiGrid, cellID, 2, 2, 1));
      }
      
      
      for(unsigned int i=0;i<SC->neighbors.size();i++){
         if (SC->neighbors[i] == INVALID_CELLID) {
      // Offsets to missing 
      // neighbours are replaced by offsets to this cell so that cells on the 
      // boundary of the simulation domain (="ghost cells") can be propagated.
      // This allows boundary values (set by user) to propagate into simulation 
      // domain.
            SC->neighbors[i]=cellID;
         }
         else
            SC->procBoundaryFlag = SC->procBoundaryFlag | (1 << i);
      }
   }
   
   // ***** Calculate MPI send/receive stencils *****
   // Send/receive stencils for avgs:
   stencilAverages.clear();

   vector<Offset> nbrOffsets;   
   nbrOffsets.push_back(Offset(-1, 0, 0));
   nbrOffsets.push_back(Offset( 1, 0, 0));
   nbrOffsets.push_back(Offset( 0,-1, 0));
   nbrOffsets.push_back(Offset( 0, 1, 0));
   nbrOffsets.push_back(Offset( 0, 0,-1));
   nbrOffsets.push_back(Offset( 0, 0, 1));
   nbrOffsets.push_back(Offset(-2, 0, 0));
   nbrOffsets.push_back(Offset( 0,-2, 0));
   nbrOffsets.push_back(Offset( 0, 0,-2));
   stencilAverages.addReceives(mpiGrid,nbrOffsets);
   nbrOffsets.clear();

   nbrOffsets.push_back(Offset(-1, 0, 0));
   nbrOffsets.push_back(Offset( 1, 0, 0));
   nbrOffsets.push_back(Offset( 0,-1, 0));
   nbrOffsets.push_back(Offset( 0, 1, 0));
   nbrOffsets.push_back(Offset( 0, 0,-1));
   nbrOffsets.push_back(Offset( 0, 0, 1));
   nbrOffsets.push_back(Offset( 2, 0, 0));
   nbrOffsets.push_back(Offset( 0, 2, 0));
   nbrOffsets.push_back(Offset( 0, 0, 2));
   stencilAverages.addSends(mpiGrid,nbrOffsets);
   nbrOffsets.clear();

      // Send/receive stencils for df/dt updates:

   stencilUpdates.clear();
   for (int k=-1; k<2; ++k) for (int j=-1; j<2; ++j) for (int i=-1; i<2; ++i) {
      if (i == 0 && j == 0 && k == 0) {
           continue;
      }
      nbrOffsets.push_back(Offset(i,j,k));
   }
   stencilUpdates.addRemoteUpdateReceives(mpiGrid,nbrOffsets);
   stencilUpdates.addRemoteUpdateSends(mpiGrid,nbrOffsets);
   
   // Now iterate through all local cells, and insert the cells 
   // with sysBoundaryFlag higher than NOT_SYSBOUNDARY into sysBoundaryCells list. 
   //not that sysBounadryCells does not  (anymore) contain remote cells, only local!!!
   for (uint c=0; c<cells.size(); ++c) {
      const CellID cellID = cells[c];
      if (mpiGrid[cellID]->sysBoundaryFlag != sysboundarytype::NOT_SYSBOUNDARY) sysBoundaryCells.insert(cellID);
   }
   allocateSpatialLevequeBuffers(mpiGrid);
   return true;
}
bool deallocateSpatialLevequeBuffers(){
   remoteUpdates.clear();
   updateBuffers.clear();
   return true;
}

bool allocateSpatialLevequeBuffers(const dccrg::Dccrg<SpatialCell>& mpiGrid){
   // Allocate receive buffers for all local cells that 
   // have at least one remote neighbour. For GPUs the first 
   // buffer must be allocated using page-locked memory:
   deallocateSpatialLevequeBuffers();
   
   for (map<pair<int,int>,CellID>::const_iterator it=stencilUpdates.recvs.begin(); it!=stencilUpdates.recvs.end(); ++it) {
      cint host            = it->first.first;
      const CellID localID = it->second;
      const size_t elements = mpiGrid[localID]->size()*SIZE_VELBLOCK;
      remoteUpdates[localID].push_back(vector<Real>(elements));
      updateBuffers.insert(make_pair(make_pair(localID,host), &(remoteUpdates[localID].back()[0]) ));      
   }
   return true;
}


bool finalizeMover() {return true;}


void calculateAcceleration(
   dccrg::Dccrg<SpatialCell>& mpiGrid,
   Real dt
) {

   typedef Parameters P;
   const vector<CellID> cells = mpiGrid.get_cells();
   vector<CellID> propagatedCells;
   // Iterate through all local cells and propagate distribution functions 
   // in velocity space. Ghost cells (spatial cells at the boundary of the simulation 
   // volume) do not need to be propagated:

   
//set initial cells to propagate
   for (size_t c=0; c<cells.size(); ++c) {
      SpatialCell* SC = mpiGrid[cells[c]];
      //disregard boundary cells
      //do not integrate cells with no blocks    (well, do not computes in practice)
      if(SC->sysBoundaryFlag == sysboundarytype::NOT_SYSBOUNDARY &&
         SC->number_of_blocks != 0) {
         propagatedCells.push_back(cells[c]);
      }
   }
   
   //Semilagrangian acceleration
   
   phiprof::start("semilag-acc");
#pragma omp parallel for schedule(dynamic,1)
   for (size_t c=0; c<propagatedCells.size(); ++c) {
      const CellID cellID = propagatedCells[c];
      phiprof::start("cell-semilag-acc");
      cpu_accelerate_cell(mpiGrid[cellID],dt);
      phiprof::stop("cell-semilag-acc");
   }
   phiprof::stop("semilag-acc");
   
   phiprof::start("Compute moments");
#pragma omp parallel for
   for (size_t c=0; c<cells.size(); ++c) {
      const CellID cellID = cells[c];
      //compute moments after acceleration
      mpiGrid[cellID]->parameters[CellParams::RHO_V  ] = 0.0;
      mpiGrid[cellID]->parameters[CellParams::RHOVX_V] = 0.0;
      mpiGrid[cellID]->parameters[CellParams::RHOVY_V] = 0.0;
      mpiGrid[cellID]->parameters[CellParams::RHOVZ_V] = 0.0;

      for(unsigned int block_i=0; block_i< mpiGrid[cellID]->number_of_blocks;block_i++){
         unsigned int block = mpiGrid[cellID]->velocity_block_list[block_i];         
         cpu_calcVelocityMoments(mpiGrid[cellID],block,CellParams::RHO_V,CellParams::RHOVX_V,CellParams::RHOVY_V,CellParams::RHOVZ_V);   //set moments after acceleration
      }
   }
   phiprof::stop("Compute moments");
}





void calculateSpatialFluxes(dccrg::Dccrg<SpatialCell>& mpiGrid,
                            const SysBoundary& sysBoundaries,
                            creal dt) {
   typedef Parameters P;
   std::vector<MPI_Request> MPIrecvRequests;               /**< Container for active MPI_Requests due to receives.*/
   std::vector<MPI_Request> MPIsendRequests;               /**< Container for active MPI_Requests due to sends.*/
   std::vector<MPI_Datatype> MPIrecvTypes;               /**< Container for active datatypes due to receives    .*/
   std::vector<MPI_Datatype> MPIsendTypes;               /**< Container for active datatypes due to sends.*/
   
   /*
   // TEMPORARY SOLUTION
   vector<CellID> cells = mpiGrid.get_cells();
   cuint avgsByteSize = mpiGrid[cells[0]]->N_blocks * SIZE_VELBLOCK * sizeof(Real);
   // END TEMPORARY SOLUTION
    */
   phiprof::start("calculateSpatialFluxes");
   MPIsendRequests.clear(); // Note: unnecessary
   MPIrecvRequests.clear(); // Note: unnecessary

   
   // Post receives for avgs:
   phiprof::initializeTimer("Start receives","MPI");
   phiprof::start("Start receives");
   int ops=0;
   for (map<pair<int,int>,CellID>::iterator it=stencilAverages.recvs.begin(); it!=stencilAverages.recvs.end(); ++it) {
      cint host           = it->first.first;
      cint tag            = it->first.second;
      const CellID cellID = it->second;
      //cuint byteSize      = avgsByteSize; // NOTE: N_blocks should be ok in buffer cells
      mpiGrid[cellID]->set_mpi_transfer_type(Transfer::VEL_BLOCK_DATA);


      // get datatype from current cell
      MPI_Datatype temp_type;
      MPIrecvTypes.push_back(temp_type);
      void* address = NULL;
      int count = -1;
      mpiGrid[cellID]->mpi_datatype(
         address,
         count,
         MPIrecvTypes.back(),
         cellID,
         mpiGrid.get_rank(),
         mpiGrid.get_rank(),
         true
      );

      MPI_Type_commit(&(MPIrecvTypes.back()));
      MPIrecvRequests.push_back(MPI_Request());
      MPI_Irecv(mpiGrid[cellID],1,MPIrecvTypes.back(),host,tag,MPI_COMM_WORLD,&(MPIrecvRequests.back()));      
      ops++;
   }
   phiprof::stop("Start receives",ops,"receives");

   // Post sends for avgs:
   phiprof::initializeTimer("Start sends","MPI");
   ops=0;
   phiprof::start("Start sends");
   for (multimap<CellID,pair<int,int> >::iterator it=stencilAverages.sends.begin(); it!=stencilAverages.sends.end(); ++it) {
      const CellID cellID = it->first;
      cint host           = it->second.first;
      cint tag            = it->second.second;
      mpiGrid[cellID]->set_mpi_transfer_type(Transfer::VEL_BLOCK_DATA);
      // get datatype from current cell
      MPI_Datatype temp_type;
      MPIsendTypes.push_back(temp_type);
      void* address = NULL;
      int count = -1;
      mpiGrid[cellID]->mpi_datatype(
         address,
         count,
         MPIsendTypes.back(),
         cellID,
         mpiGrid.get_rank(),
         mpiGrid.get_rank(),
         false
      );
      MPI_Type_commit(&(MPIsendTypes.back()));

      MPIsendRequests.push_back(MPI_Request());      
      if (MPI_Isend(mpiGrid[cellID],1,MPIsendTypes.back(),host,tag,MPI_COMM_WORLD,&(MPIsendRequests.back())) != MPI_SUCCESS) {
         std::cerr << "calculateSpatialFlux failed to send data!" << std::endl;
      }
      ops++;
      // FIXME: don't leak memory - free the committed datatype
   }
   phiprof::stop("Start sends",ops,"sends");
   
   // Clear spatial fluxes to zero value. Remote neighbour df/dt arrays 
   // need to be cleared as well:
   vector<CellID> cells=mpiGrid.get_cells();
   const vector<CellID> remoteCells
      = mpiGrid.get_remote_cells_on_process_boundary(VLASOV_SOLVER_FLUXES_NEIGHBORHOOD_ID);
   cells.insert( cells.end(), remoteCells.begin(), remoteCells.end() );
   
   phiprof::start("Mark unitialized flux");
#pragma omp  parallel for
   for (size_t c=0; c<cells.size(); ++c) {
      const CellID cellID = cells[c];
      SpatialCell* SC=mpiGrid[cellID];
      if(SC->sysBoundaryFlag == sysboundarytype::DO_NOT_COMPUTE ||
         (SC->sysBoundaryLayer != 1  &&
             SC->sysBoundaryFlag != sysboundarytype::NOT_SYSBOUNDARY)
      ) continue;
      const Real dx=SC->parameters[CellParams::DX];
      const Real dy=SC->parameters[CellParams::DY];
      const Real dz=SC->parameters[CellParams::DZ];

      //Reset limit from spatial space
      SC->parameters[CellParams::MAXRDT]=numeric_limits<Real>::max();

      for(unsigned int block_i=0; block_i< SC->number_of_blocks;block_i++){
         unsigned int block = SC->velocity_block_list[block_i];
         Velocity_Block* block_ptr = SC->at(block);
         const Real* const blockParams = block_ptr->parameters;
         
         //mark that this block is uninitialized
         block_ptr->fx[0] = numeric_limits<Real>::max();

         //compute maximum dt. In separate loop here, as the propagation
         //loops are parallelized over blocks, which would force us to
         //add critical regions.
         //loop over max/min velocity cells in block
         for (unsigned int i=0; i<WID;i+=WID-1) {
            const Real Vx = blockParams[BlockParams::VXCRD] + (i+HALF)*blockParams[BlockParams::DVX];
            const Real Vy = blockParams[BlockParams::VYCRD] + (i+HALF)*blockParams[BlockParams::DVY];
            const Real Vz = blockParams[BlockParams::VZCRD] + (i+HALF)*blockParams[BlockParams::DVZ];
            
            if(fabs(Vx)!=ZERO) SC->parameters[CellParams::MAXRDT]=min(dx/fabs(Vx),SC->parameters[CellParams::MAXRDT]);
            if(fabs(Vy)!=ZERO) SC->parameters[CellParams::MAXRDT]=min(dy/fabs(Vy),SC->parameters[CellParams::MAXRDT]);
            if(fabs(Vz)!=ZERO) SC->parameters[CellParams::MAXRDT]=min(dz/fabs(Vz),SC->parameters[CellParams::MAXRDT]);
         }
         
      }
   }
   phiprof::stop("Mark unitialized flux");
   
   // Iterate through all local cells and calculate their contributions to 
   // time derivatives of distribution functions df/dt in spatial space. Ghost cell 
   // (spatial cells at the boundary of the simulation volume) contributions need to be
   // calculated as well:
   phiprof::start("df/dt in real space (inner)");

   
#pragma omp parallel
   for (set<CellID>::iterator cell=stencilAverages.innerCells.begin(); cell!=stencilAverages.innerCells.end(); ++cell) {
      SpatialCell* SC = mpiGrid[*cell];
      if(SC->sysBoundaryFlag == sysboundarytype::DO_NOT_COMPUTE ||
         (SC->sysBoundaryLayer != 1  &&
             SC->sysBoundaryFlag != sysboundarytype::NOT_SYSBOUNDARY )) continue;
      // Iterate through all velocity blocks in the spatial cell and calculate 
      // contributions to df/dt:
#pragma omp for
      for(unsigned int block_i=0; block_i< SC->number_of_blocks;block_i++){
         unsigned int block = SC->velocity_block_list[block_i];
         cpu_calcSpatDfdt(mpiGrid,SC,block,dt);
      }
   }
   phiprof::stop("df/dt in real space (inner)");

   // Wait for remote avgs:
   phiprof::initializeTimer("Wait receives","MPI","Wait");
   phiprof::start("Wait receives");
   // Wait for all receives to complete:
   MPI_Waitall(MPIrecvRequests.size(),&(MPIrecvRequests[0]),MPI_STATUSES_IGNORE);
   // Free memory:
   MPIrecvRequests.clear();
   for(unsigned int i=0;i<MPIrecvTypes.size();i++){
      MPI_Type_free(&(MPIrecvTypes[i]));
   }
   MPIrecvTypes.clear();
   phiprof::stop("Wait receives");
   
   // Iterate through the rest of local cells:
   phiprof::start("df/dt in real space (boundary)");


#pragma omp parallel 
   for (set<CellID>::iterator cell=stencilAverages.boundaryCells.begin(); cell!=stencilAverages.boundaryCells.end(); ++cell) {
      SpatialCell* SC = mpiGrid[*cell];
      if(SC->sysBoundaryFlag == sysboundarytype::DO_NOT_COMPUTE ||
         (SC->sysBoundaryLayer != 1  &&
             SC->sysBoundaryFlag != sysboundarytype::NOT_SYSBOUNDARY )
      ) continue;
      // Iterate through all velocity blocks in the spatial cell and calculate 
      // contributions to df/dt:
#pragma omp for
      for(unsigned int block_i=0; block_i< SC->number_of_blocks;block_i++){
         unsigned int block = SC->velocity_block_list[block_i];         
         cpu_calcSpatDfdt(mpiGrid,SC,block,dt);
      }
   }
   phiprof::stop("df/dt in real space (boundary)");

   phiprof::start("zero fluxes");
#pragma omp  parallel for
   for (size_t c=0; c<cells.size(); ++c) {
      const CellID cellID = cells[c];
      SpatialCell* SC=mpiGrid[cellID];
      if(SC->sysBoundaryFlag == sysboundarytype::DO_NOT_COMPUTE ||
         (SC->sysBoundaryLayer != 1  &&
             SC->sysBoundaryFlag != sysboundarytype::NOT_SYSBOUNDARY )
      ) continue;
      for(unsigned int block_i=0; block_i< SC->number_of_blocks;block_i++){
         unsigned int block = SC->velocity_block_list[block_i];
         Velocity_Block* block_ptr = SC->at(block);
         //zero block that was not touched
         if(block_ptr->fx[0] == numeric_limits<Real>::max()){
            for (uint i=0; i<SIZE_VELBLOCK; i++) block_ptr->fx[i]=0.0;
         }
      } 
   } 
   phiprof::stop("zero fluxes");

   
   // Wait for sends to complete:
   phiprof::initializeTimer("Wait sends","MPI","Wait");
   phiprof::start("Wait sends");

   MPI_Waitall(MPIsendRequests.size(),&(MPIsendRequests[0]),MPI_STATUSES_IGNORE);

   MPIsendRequests.clear();
   for(unsigned int i=0;i<MPIsendTypes.size();i++){
      MPI_Type_free(&(MPIsendTypes[i]));
   }
   MPIsendTypes.clear();
   phiprof::stop("Wait sends");
   phiprof::stop("calculateSpatialFluxes");
}

void calculateSpatialPropagation(dccrg::Dccrg<SpatialCell>& mpiGrid) { 
   std::vector<MPI_Request> MPIrecvRequests;               /**< Container for active MPI_Requests due to receives.*/
   std::vector<MPI_Request> MPIsendRequests;               /**< Container for active MPI_Requests due to sends.*/
   std::vector<MPI_Datatype> MPIsendTypes;               /**< Container for active datatypes due to sends.*/
   
   phiprof::start("calculateSpatialPropagation");
//    vector<CellID> cells;
   vector<CellID> innerCellIds;
   for (set<CellID>::iterator c=stencilUpdates.innerCells.begin(); c!=stencilUpdates.innerCells.end(); ++c) {
       innerCellIds.push_back(*c);
   }

   vector<CellID> boundaryCellIds;
   for (set<CellID>::iterator c=stencilUpdates.boundaryCells.begin(); c!=stencilUpdates.boundaryCells.end(); ++c) {
       boundaryCellIds.push_back(*c);
   }
   
// Post receives for remote updates:
   

//    cells=mpiGrid.get_cells();
   
   MPIsendRequests.clear(); 
   MPIrecvRequests.clear();
   MPIsendTypes.clear();
   phiprof::initializeTimer("Start receives","MPI");
   phiprof::start("Start receives");


   

   int ops=0;

   for (map<pair<int,int>,CellID>::const_iterator it=stencilUpdates.recvs.begin(); it!=stencilUpdates.recvs.end(); ++it) {
      const CellID localID  = it->second;
      cint host             = it->first.first;
      cint tag              = it->first.second;
      
      map<pair<CellID,int>,Real*>::iterator it2 = updateBuffers.find(make_pair(localID,host));
      if (it2 == updateBuffers.end()) {cerr << "FATAL ERROR: Could not find update buffer!" << endl; exit(1);}
      char* const buffer    = reinterpret_cast<char*>(it2->second);

      //receive as bytestream (convert to sparse format later on)       
      MPIrecvRequests.push_back(MPI_Request());
      MPI_Irecv(buffer, mpiGrid[localID]->size()*SIZE_VELBLOCK*sizeof(Real),MPI_BYTE,host,tag,MPI_COMM_WORLD,&(MPIrecvRequests.back()));
      ops++;
   }
   phiprof::stop("Start receives",ops,"receives");
   phiprof::initializeTimer("Start sends","MPI");
   phiprof::start("Start sends");
   ops=0;
// Post sends for remote updates:
   for (multimap<CellID,pair<int,int> >::const_iterator it=stencilUpdates.sends.begin(); it!=stencilUpdates.sends.end(); ++it) {
      const CellID nbrID    = it->first;
      cint host             = it->second.first;
      cint tag              = it->second.second;
      mpiGrid[nbrID]->set_mpi_transfer_type(Transfer::VEL_BLOCK_FLUXES);

      // get datatype from current cell
      MPI_Datatype temp_type;
      MPIsendTypes.push_back(temp_type);
      void* address = NULL;
      int count = -1;
      mpiGrid[nbrID]->mpi_datatype(
         address,
         count,
         MPIsendTypes.back(),
         nbrID,
         mpiGrid.get_rank(),
         mpiGrid.get_rank(),
         false
      );


      MPI_Type_commit(&(MPIsendTypes.back()));
      
      MPIsendRequests.push_back(MPI_Request());
      if (MPI_Isend(mpiGrid[nbrID],1,MPIsendTypes.back(),host,tag,MPI_COMM_WORLD,&(MPIsendRequests.back())) != MPI_SUCCESS) {   
         std::cerr << "calculateSpatialPropagation failed to send data!" << std::endl;
      }
      ops++;
   }
   phiprof::stop("Start sends",ops,"sends");

   phiprof::start("Spatial trans (inner)");
//cpu_propagetSpatWithMoments only write to data        in cell cellID, parallel for safe

#pragma omp parallel for schedule(guided)
   for (unsigned int i=0;i<innerCellIds.size();i++){
      const CellID cellID = innerCellIds[i];

      creal* const nbr_dfdt    = NULL;
      SpatialCell* SC = mpiGrid[cellID];
      // Clear velocity moments that have been calculated during the previous time step:
      SC->parameters[CellParams::RHO_R] = 0.0;
      SC->parameters[CellParams::RHOVX_R] = 0.0;
      SC->parameters[CellParams::RHOVY_R] = 0.0;
      SC->parameters[CellParams::RHOVZ_R] = 0.0;
      
      // Propagate and compute moments for non-sysboundary cells
      if (sysBoundaryCells.find(cellID) == sysBoundaryCells.end()) {
         for(unsigned int block_i=0; block_i< SC->number_of_blocks;block_i++){
            unsigned int block = SC->velocity_block_list[block_i];         
            cpu_propagateSpatWithMoments(nbr_dfdt,SC,block,block_i);
         }
      }
   }

   phiprof::stop("Spatial trans (inner)");

   
   // Wait for remote neighbour updates to arrive:
   phiprof::initializeTimer("Wait receives","MPI","Wait");
   phiprof::start("Wait receives");
   // Wait for all receives to complete:
   MPI_Waitall(MPIrecvRequests.size(),&(MPIrecvRequests[0]),MPI_STATUSES_IGNORE);
   // Free memory:
   MPIrecvRequests.clear();
   phiprof::stop("Wait receives");

      
   // Sum remote neighbour updates to the first receive buffer of each 
   // local cell (if necessary):

   phiprof::start("Sum remote updates");
#pragma omp parallel
   for (map<CellID,vector<vector<Real> > >::iterator it=remoteUpdates.begin(); it!=remoteUpdates.end(); ++it) {
      const CellID cellID = it->first;
      vector<vector<Real> >::iterator buffer = it->second.begin();
      //sum results into first receive buffer if is not empty
      if(buffer != it->second.end()) {
         Real* sumBuffer = &((*buffer)[0]);
         ++buffer;
         while (buffer != it->second.end()) {
#pragma omp for
            for (uint i=0; i< (*buffer).size();i++)
               sumBuffer[i] += (*buffer)[i];
            ++buffer;
         }
      }
   }
   phiprof::stop("Sum remote updates");
   phiprof::start("Spatial trans (boundary)");
   
   // Propagate boundary cells:
//cpu_propagetSpatWithMoments only write to data in cell cellID, parallel for safe
#pragma omp parallel for  schedule(guided)
   for (unsigned int i=0;i<boundaryCellIds.size();i++){
      const CellID cellID = boundaryCellIds[i];
      creal* const nbr_dfdt    = &(remoteUpdates[cellID][0][0]);
      SpatialCell* SC = mpiGrid[cellID];
      SC->parameters[CellParams::RHO_R] = 0.0;
      SC->parameters[CellParams::RHOVX_R] = 0.0;
      SC->parameters[CellParams::RHOVY_R] = 0.0;
      SC->parameters[CellParams::RHOVZ_R] = 0.0;
      
      
      // Do not propagate or compute moments for sysboundary cells
      if (sysBoundaryCells.find(cellID) == sysBoundaryCells.end()) {
         for(unsigned int block_i=0; block_i< SC->number_of_blocks;block_i++){
            unsigned int block = SC->velocity_block_list[block_i];
            cpu_propagateSpatWithMoments(nbr_dfdt,SC,block,block_i);
         }
         
      }
   }
   phiprof::stop("Spatial trans (boundary)");

   // Wait for neighbour update sends:
   phiprof::initializeTimer("Wait sends","MPI","Wait");
   phiprof::start("Wait sends");

   MPI_Waitall(MPIsendRequests.size(),&(MPIsendRequests[0]),MPI_STATUSES_IGNORE);

   // Free memory:
   MPIsendRequests.clear();
   for(unsigned int i=0;i<MPIsendTypes.size();i++){
      MPI_Type_free(&(MPIsendTypes[i]));
   }
   MPIsendTypes.clear();
   
   phiprof::stop("Wait sends");
   phiprof::stop("calculateSpatialPropagation");
}



void calculateInterpolatedVelocityMoments(dccrg::Dccrg<SpatialCell>& mpiGrid,
                                          const int cp_rho, const int cp_rhovx, const int cp_rhovy, const int cp_rhovz) {
   vector<CellID> cells;
   cells=mpiGrid.get_cells();
   
   //Iterate through all local cells (excl. system boundary cells):
#pragma omp parallel for
   for (size_t c=0; c<cells.size(); ++c) {
      const CellID cellID = cells[c];
      SpatialCell* SC = mpiGrid[cellID];
      if(SC->sysBoundaryFlag == sysboundarytype::NOT_SYSBOUNDARY) {
         SC->parameters[cp_rho  ] = 0.5* ( SC->parameters[CellParams::RHO_R] + SC->parameters[CellParams::RHO_V] );
         SC->parameters[cp_rhovx] = 0.5* ( SC->parameters[CellParams::RHOVX_R] + SC->parameters[CellParams::RHOVX_V] );
         SC->parameters[cp_rhovy] = 0.5* ( SC->parameters[CellParams::RHOVY_R] + SC->parameters[CellParams::RHOVY_V] );
         SC->parameters[cp_rhovz] = 0.5* ( SC->parameters[CellParams::RHOVZ_R] + SC->parameters[CellParams::RHOVZ_V] );
      
      }
   }
}




void calculateCellVelocityMoments(
   SpatialCell* SC,
   bool doNotSkip
){
   if(!doNotSkip &&
         (SC->sysBoundaryFlag == sysboundarytype::DO_NOT_COMPUTE ||
            (SC->sysBoundaryLayer != 1  &&
             SC->sysBoundaryFlag != sysboundarytype::NOT_SYSBOUNDARY))
   ) return;
   SC->parameters[CellParams::RHO  ] = 0.0;
   SC->parameters[CellParams::RHOVX] = 0.0;
   SC->parameters[CellParams::RHOVY] = 0.0;
   SC->parameters[CellParams::RHOVZ] = 0.0;
   //Iterate through all velocity blocks in this spatial cell 
   // and calculate velocity moments:
   for(unsigned int block_i=0; block_i< SC->number_of_blocks;block_i++){
      unsigned int block = SC->velocity_block_list[block_i];
      cpu_calcVelocityMoments(SC,block,CellParams::RHO,CellParams::RHOVX,CellParams::RHOVY,CellParams::RHOVZ);
   }
}
 

void calculateVelocityMoments(dccrg::Dccrg<SpatialCell>& mpiGrid){
   vector<CellID> cells;
   cells=mpiGrid.get_cells();
   phiprof::start("Calculate moments"); 
   // Iterate through all local cells (incl. system boundary cells):
#pragma omp parallel for
   for (size_t c=0; c<cells.size(); ++c) {
      const CellID cellID = cells[c];
      SpatialCell* SC = mpiGrid[cellID];
      calculateCellVelocityMoments(SC);
   }
   phiprof::stop("Calculate moments"); 
}
 

