#include <OBFunctionTerm>

#define __CL_ENABLE_EXCEPTIONS
#define __NO_STD_VECTOR
#define __NO_STD_STRING
#include "cl.hpp"

namespace OpenBabel {
namespace OBFFs {

  class MMFF94Common;

  class MMFF94ElectroTermOpenCL : public OBFunctionTerm
  {
    public:
      MMFF94ElectroTermOpenCL(OBFunction *function, MMFF94Common *common);
      std::string GetName() const { return "MMFF94 electrostatic term (OpenCL)"; }
      bool Setup(/*const*/ OBMol &molecule);
      void Compute(OBFunction::Computation computation = OBFunction::Value);
      double GetValue() const { return m_value; }
    protected:
      void InitSelfPairs(OBMol &);
      double ComputeTotalEnergy();
      double ComputeSelfEnergy();
      double ComputeSelfGradients();

      MMFF94Common *m_common;
      double m_value;
      unsigned int m_numAtoms;

      std::vector<std::pair<unsigned int, unsigned int> > m_selfPairs;
      std::vector<std::pair<unsigned int, unsigned int> > m_oneFourPairs;

      // OpenCL
      cl::Context m_context;
      cl::Program m_program;
      cl::Kernel m_kernel;
      cl::CommandQueue m_queue;
      cl::Buffer m_devPos, m_devGrad;
  };

} // OBFFs
} // OpenBabel
