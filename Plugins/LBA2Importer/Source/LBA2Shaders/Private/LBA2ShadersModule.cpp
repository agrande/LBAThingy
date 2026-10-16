#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

/**
 * Maps the plugin's Shaders folder to /Plugin/LBA2Importer so materials can include its HLSL (Custom nodes of the
 * generated post-process materials). Shader paths must be registered before shaders compile, hence its own module
 * loaded at PostConfigInit.
 */
class FLBA2ShadersModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LBA2Importer"));
		if (Plugin.IsValid())
		{
			AddShaderSourceDirectoryMapping(TEXT("/Plugin/LBA2Importer"), FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));
		}
	}
};

IMPLEMENT_MODULE(FLBA2ShadersModule, LBA2Shaders)
