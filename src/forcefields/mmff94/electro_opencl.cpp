/*********************************************************************
MMFF94ElectroTermOpenCL - MMFF94 force field bond stratching term

Copyright (C) 2006-2008,2009 by Tim Vandermeersch
 
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

#include "electro_opencl.h"
#include "parameter.h"
#include "common.h"

#include <openbabel/mol.h>

#include <OBLogFile>
#include <OBVectorMath>

using namespace std;

namespace OpenBabel {
namespace OBFFs {
 
  struct Parameter {
    enum { Class, TypeA, TypeB, TypeC, Ka, Theta0 };
  };
 
  MMFF94ElectroTermOpenCL::MMFF94ElectroTermOpenCL(OBFunction *function, MMFF94Common *common) : OBFunctionTerm(function), m_common(common)
  {
    m_value = 999999.99;
  }
   
  void MMFF94ElectroTermOpenCL::Compute(OBFunction::Computation computation)
  {
    //cout << "MMFF94ElectroTermOpenCL::Compute" << endl;
    m_value = 0.0;
      

    const std::vector<double> &charges = m_common->m_pCharges;
    // setup input (posistions + charges) to copy to device
    cl_float hostData[4*m_numAtoms];
    for (int i = 0; i < m_numAtoms; ++i) {
      int offset = 4 * i;
      const Eigen::Vector3d &atomPos = m_function->GetPositions().at(i);
      hostData[offset  ] = atomPos.x();
      hostData[offset+1] = atomPos.y();
      hostData[offset+2] = atomPos.z();
      hostData[offset+3] = charges.at(i);
    }

    int p = 256;
    int globalWorkSize = m_numAtoms + p - m_numAtoms % p;
 
    try {
      // write positions (xyz) and charges (w) to device
      m_queue.enqueueWriteBuffer(m_devPos, CL_TRUE, 0, 4 * m_numAtoms * sizeof(cl_float), (void*)hostData);

      // execute kernel
      cl::KernelFunctor func = m_kernel.bind(m_queue, cl::NDRange(m_numAtoms), cl::NullRange);
      cl::LocalSpaceArg sharedPos = cl::__local(4 * p * sizeof(cl_float));
      func(m_devPos, m_devGrad, m_numAtoms, sharedPos);

      // read back gradients (xyz) and energies (w) from device
      m_queue.enqueueReadBuffer(m_devGrad, CL_TRUE, 0, 4 * m_numAtoms * sizeof(cl_float), (void*)hostData);

    } catch (cl::Error err) {
      std::cout << "  ERROR: " << err.what() << "(" << err.err() << ")" << std::endl;
    }

    // sum the energies
    double devEnergy = 0.0;
    for (int i = 0; i < m_numAtoms; i++) {
      int offset = 4*i;
      m_function->GetGradients()[i].x() += hostData[offset];
      m_function->GetGradients()[i].y() += hostData[offset+1];
      m_function->GetGradients()[i].z() += hostData[offset+2];
//      cout << i  << " = pos(" << hostData[offset] << ", " << hostData[offset+1] << ", " << hostData[offset+2] << ")  q = " << hostData[offset+3] << std::endl;
      devEnergy += hostData[offset+3];
    }
    //cout << "devEnergy = " << devEnergy << endl;

    // compute the self energy (i.e. i == j, bonded atoms, 1-3 (angle terminal atoms) and the 1-4 atoms contribution times 0.25
    double selfEnergy;
    if (computation == OBFunction::Gradients)
      selfEnergy = ComputeSelfGradients();
    else
      selfEnergy = ComputeSelfEnergy();

    m_value = 0.5 * 332.0716 * (devEnergy - selfEnergy);

    OBLogFile *logFile = m_function->GetLogFile();
    if (logFile->IsMedium()) {
      std::stringstream ss;
      ss << "     TOTAL ELECTROSTATIC ENERGY = " << m_value << " " << m_function->GetUnit() << std::endl;
      logFile->Write(ss.str());
    }
 
    //cout << "E_ele = " << m_value << endl;
  }

  void MMFF94ElectroTermOpenCL::InitSelfPairs(OBMol &mol)
  {
    unsigned int numAtoms = mol.NumAtoms();

    for (unsigned int i = 0; i < numAtoms; ++i) {
      for (unsigned int j = 0; j < numAtoms; ++j) {
        if (i == j) {
          m_selfPairs.push_back(std::pair<unsigned int, unsigned int>(i, j));
          continue;
        }

        OBAtom *a = mol.GetAtom(i+1);
        OBAtom *b = mol.GetAtom(j+1);

        if (a->IsConnected(b))
          m_selfPairs.push_back(std::pair<unsigned int, unsigned int>(i, j));
        else if (a->IsOneThree(b))
          m_selfPairs.push_back(std::pair<unsigned int, unsigned int>(i, j));
        else if (a->IsOneFour(b) )
          m_oneFourPairs.push_back(std::pair<unsigned int, unsigned int>(i, j));
      }
    }
  }

  double electroEnergy(const Eigen::Vector3d &ai, const Eigen::Vector3d &aj, double qi, double qj)
  {
    Eigen::Vector3d r = ai - aj;
    double dist = r.norm() + 0.05;
    double e = qi * qj / dist;
  }

  double MMFF94ElectroTermOpenCL::ComputeTotalEnergy()
  {
    const std::vector<double> &charges = m_common->m_pCharges;
    int numAtoms = charges.size();

    double E = 0.0;
    for (int i = 0; i < numAtoms; ++i) {
      for (int j = 0; j < numAtoms; ++j) {
        const Eigen::Vector3d &ai = m_function->GetPositions().at(i);
        const Eigen::Vector3d &aj = m_function->GetPositions().at(j);
        E += electroEnergy(ai, aj, charges.at(i), charges.at(j));
      }
    }

    return E;  
  }

  double MMFF94ElectroTermOpenCL::ComputeSelfEnergy()
  {
    const std::vector<double> &charges = m_common->m_pCharges;
    int numAtoms = charges.size();

    double E = 0.0;
    for (unsigned int k = 0; k < m_selfPairs.size(); ++k) {
      unsigned int i = m_selfPairs.at(k).first;
      unsigned int j = m_selfPairs.at(k).second;
      const Eigen::Vector3d &ai = m_function->GetPositions().at(i);
      const Eigen::Vector3d &aj = m_function->GetPositions().at(j);
          
      E += electroEnergy(ai, aj, charges.at(i), charges.at(j));
    }

    for (unsigned int k = 0; k < m_oneFourPairs.size(); ++k) {
      unsigned int i = m_oneFourPairs.at(k).first;
      unsigned int j = m_oneFourPairs.at(k).second;
      const Eigen::Vector3d &ai = m_function->GetPositions().at(i);
      const Eigen::Vector3d &aj = m_function->GetPositions().at(j);
          
      E += 0.25 * electroEnergy(ai, aj, charges.at(i), charges.at(j));
    }

    //cout << "E_self = " << E << endl;
    return E;  
  }

  double MMFF94ElectroTermOpenCL::ComputeSelfGradients()
  {
    const std::vector<double> &charges = m_common->m_pCharges;
    int numAtoms = charges.size();

    double E = 0.0;
    for (unsigned int k = 0; k < m_selfPairs.size(); ++k) {
      unsigned int i = m_selfPairs.at(k).first;
      unsigned int j = m_selfPairs.at(k).second;
      const Eigen::Vector3d &ai = m_function->GetPositions().at(i);
      const Eigen::Vector3d &aj = m_function->GetPositions().at(j);     
      
      Eigen::Vector3d r = ai - aj;
      double dist = r.norm() + 0.05;
      double dist2 = dist * dist;

      double QiQj = charges.at(i) * charges.at(j);

      double e = QiQj / dist;
      double dE = 332.0716 * QiQj / (dist2 * dist);

      m_function->GetGradients()[i] += r * dE;
      m_function->GetGradients()[j] -= r * dE;

      E += e;
    }

    for (unsigned int k = 0; k < m_oneFourPairs.size(); ++k) {
      unsigned int i = m_oneFourPairs.at(k).first;
      unsigned int j = m_oneFourPairs.at(k).second;
      const Eigen::Vector3d &ai = m_function->GetPositions().at(i);
      const Eigen::Vector3d &aj = m_function->GetPositions().at(j);

      Eigen::Vector3d r = ai - aj;
      double dist = r.norm() + 0.05;
      double dist2 = dist * dist;

      double QiQj = charges.at(i) * charges.at(j);

      double e = QiQj / dist;
      double dE = - 0.25 * 332.0716 * QiQj / dist2;
    
      m_function->GetGradients()[i] += r * dE;
      m_function->GetGradients()[j] -= r * dE;

      E += 0.25 * e;
    }

    //cout << "E_self = " << E << endl;
    return E;  
  }



  bool MMFF94ElectroTermOpenCL::Setup(/*const*/ OBMol &mol)
  {
    OBAtom *a, *b, *c;

    OBLogFile *logFile = m_function->GetLogFile();
    OBParameterDB *database = m_function->GetParameterDB();

    if (logFile->IsLow())
      logFile->Write("SETTING UP ELECTROSTATIC CALCULATIONS...\n");
    std::stringstream ss;


    m_numAtoms = mol.NumAtoms();
    InitSelfPairs(mol);

    try {
      m_context = cl::Context(CL_DEVICE_TYPE_GPU, 0, NULL, NULL);
      cl::vector<cl::Device> devices = m_context.getInfo<CL_CONTEXT_DEVICES>();
      ss << "  # OpenCL devices: " << devices.size() << std::endl;

      if (devices.empty())
        return false;

      // Print some info about the device
      ss << "  device 1:" << std::endl;
      cl_uint ret_uint;
      size_t ret_size;
      size_t ret_psize[3];
      devices[0].getInfo(CL_DEVICE_MAX_COMPUTE_UNITS, &ret_uint);
      ss << "    CL_DEVICE_MAX_COMPUTE_UNITS = " << ret_uint << std::endl;
      devices[0].getInfo(CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS, &ret_uint);
      ss << "    CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS = " << ret_uint << std::endl;
      devices[0].getInfo(CL_DEVICE_MAX_WORK_ITEM_SIZES, &ret_psize);
      ss << "    CL_DEVICE_MAX_WORK_ITEM_SIZESS = (" << ret_psize[0] << ", " << ret_psize[1] << ", " << ret_psize[2] << ")" << std::endl;
      devices[0].getInfo(CL_DEVICE_MAX_WORK_GROUP_SIZE, &ret_size);
      ss << "    CL_DEVICE_MAX_WORK_GROUP_SIZE = " << ret_size << std::endl;

      // Open the kernel source file
      std::ifstream ifs;
      ifs.open("kernel.cl");
      if (!ifs) {
        std::stringstream msg;
        msg << "Cannot open kernel.cl..." << std::endl;
        logFile->Write(msg.str());
        return false;
      }

      // Read the file into a string
      std::string srcCode;
      std::string line;
      while (std::getline(ifs, line)) {
        srcCode += line + "\n";
      }
      ifs.close();

      // Create the OpenCL program 
      cl::Program::Sources source(1, std::make_pair(srcCode.c_str(), srcCode.size()));
      cl::Program program = cl::Program(m_context, source);
      program.build(devices);

      // Create the kernel
      m_kernel = cl::Kernel(program, "electrostaticKernel");
      m_queue = cl::CommandQueue(m_context, devices[0], 0);

      //m_devPos = cl::Buffer(m_context, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, 4 * m_numAtoms * sizeof(cl_float), (void*)hostData);
      m_devPos = cl::Buffer(m_context, CL_MEM_READ_WRITE, 4 * m_numAtoms * sizeof(cl_float));
      m_devGrad = cl::Buffer(m_context, CL_MEM_READ_WRITE, 4 * m_numAtoms * sizeof(cl_float));

    } catch (cl::Error err) {
      ss << "  ERROR: " << err.what() << "(" << err.err() << ")" << std::endl;
    }


    if (logFile->IsLow())
      logFile->Write(ss.str());

    return true;
  }       

}
} // end namespace OpenBabel

//! \file forcefieldmmff94.cpp
//! \brief MMFF94 force field
