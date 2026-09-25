// Copyright Incanta Games. All Rights Reserved.

using UnrealBuildTool;

public class RedwoodTests : ModuleRules {
  public RedwoodTests(ReadOnlyTargetRules Target) : base(Target) {
    PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

    PublicIncludePaths.AddRange(
      new string[] {
      }
    );

    PrivateIncludePaths.AddRange(
      new string[] {
      }
    );

    PublicDependencyModuleNames.AddRange(
      new string[] {
        "Core",
      }
    );

    PrivateDependencyModuleNames.AddRange(
      new string[] {
        "CoreUObject",
        "Engine",
        "UnrealEd",
        "Redwood",
        "Json",
        "SIOJson",
        // FORK(hollowed-oath): HollowedOath#2854. ReloginFailureTest makes
        // unconnected sockets to drive the reconnect state.
        "SocketIOClient",
      }
    );

    DynamicallyLoadedModuleNames.AddRange(
      new string[] {
      }
    );
  }
}
