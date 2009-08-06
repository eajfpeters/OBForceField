#include <OBFunction>
#include <OBLogFile>
#include "obtest.h"

#include <openbabel/mol.h>
#include <openbabel/obconversion.h>

using OpenBabel::OBMol;
using OpenBabel::OBConversion;

using namespace OpenBabel::OBFFs;

using namespace std;




int main()
{
  OBFunctionFactory *mmff94_factory = OBFunctionFactory::GetFactory("MMFF94");
  OB_ASSERT( mmff94_factory != 0);

  OBFunction *mmff94_function = mmff94_factory->NewInstance();
  OB_ASSERT( mmff94_function != 0);

  OBMol mol;
  OBConversion conv;
  conv.SetInFormat("sdf");

  std::ifstream ifs;
  //ifs.open("hexane.xyz");
  ifs.open("aceton.sdf");
  conv.Read(&mol, &ifs);
  ifs.close();

  cout << "num atoms = " << mol.NumAtoms() << endl;

  mmff94_function->GetLogFile()->SetOutputStream(&std::cout);

  mmff94_function->Setup(mol);
  mmff94_function->Compute();

  cout << "E{bond} = " << mmff94_function->GetValue() << endl;
  
  //  MMFF94SimpleParameterDB *database = new MMFF94SimpleParameterDB("/home/timvdm/OBForceField/laptop/data/mmff94.ff");

  

  cout << "Options:" << endl;
  cout << mmff94_function->GetOptions() << endl;
 
  std::stringstream ss;
  ss << "mmff_vdw = allpair";
  mmff94_function->SetOptions(ss.str());


}