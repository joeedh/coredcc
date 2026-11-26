#include "Modules/ModuleManager.h"

/* The spike's module: everything it does is in its automation tests (Private/Tests). */
class FCoredccSpikeModule : public IModuleInterface
{
public:
	void StartupModule() override
	{
		UE_LOG(LogTemp, Display, TEXT("coredcc spike: loaded (msc %d)"), _MSC_FULL_VER);
	}
};

IMPLEMENT_MODULE(FCoredccSpikeModule, CoredccSpike)
