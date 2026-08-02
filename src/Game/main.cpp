#include "Engine/Engine.h"
#include "Foundation/Debug.h"
#include "Foundation/Log.h"

int main()
{
   Foundation::Log::Init("logs/omdlab.log");
   Foundation::Log::Write(Foundation::Log::Severity::Info, "Game", "OmdLab starting up");

   Engine::PrintDependencyChain();

   OMD_DEBUG_PRINT("Debug print smoke test, value = %d", 42);
   OMD_ASSERT(1 + 1 == 2, "Sanity check failed: math is broken");

   Foundation::Log::Shutdown();
   return 0;
}
