#include "targets.h"

#include "resolve.h"

namespace bg3le::target {

#define BG3LE_TARGET(fn, expr) \
    std::uintptr_t fn() {                  \
        static const std::uintptr_t at = (expr); \
        return at;                        \
    }

BG3LE_TARGET(EntityStorageLookup,
             resolve_call(Sig{"EntityStorageLookup", 0x218c6d1, "e8 ?? ?? ?? ?? 84 d2 75 1a 44 89 f1 c1 e9 03 81 e1 f8 0f 00 00 48 8b 04 08 4c 0f a3 f0 0f 82 ?? ?? ?? ?? 48 8b 0c 24 0f b6 41 28"}))
BG3LE_TARGET(FixedStringCreate,
             resolve_code(Sig{"FixedStringCreate", 0x226db50, "55 41 57 41 56 41 55 41 54 53 48 81 ec c8 00 00 00 48 63 57 08"}))
BG3LE_TARGET(FlushECBs,
             resolve_code(Sig{"FlushECBs", 0x2486e10, "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 e4 c0 48 81 ec 40 0b 00 00"}))
BG3LE_TARGET(PathSearch,
             resolve_code(Sig{"PathSearch", 0x2646b80, "55 41 57 41 56 41 55 41 54 53 48 81 ec e8 02 00 00 48 89 74 24 30"}))
BG3LE_TARGET(TransformInit,
             resolve_code(Sig{"TransformInit", 0x26828a0, "41 57 41 56 53 41 89 f6 48 89 fb 44 88 b7 80 00 00 00"}))
BG3LE_TARGET(FileReaderDtor,
             resolve_code(Sig{"FileReaderDtor", 0x2703740, "41 56 53 50 0f 57 c0 48 89 fb 0f 11 47 08"}))
BG3LE_TARGET(FileReaderCtor,
             resolve_code(Sig{"FileReaderCtor", 0x2704080, "55 41 57 41 56 41 54 53 48 83 ec 10 89 d5 0f 57 c0"}))
BG3LE_TARGET(TextureManagerLoad,
             resolve_code(Sig{"TextureManagerLoad", 0x25a0770, "55 41 57 41 56 41 55 41 54 53 48 81 ec d8 00 00 00 49 89 f4 48 89 fb 44 89 44 24 1c"}))
BG3LE_TARGET(TextureManagerUnload,
             resolve_code(Sig{"TextureManagerUnload", 0x24f9570, "55 41 57 41 56 41 55 41 54 53 48 83 ec 58 49 89 f6 48 89 fb e8 ?? ?? ?? ?? 85 c0"}))
BG3LE_TARGET(StatsLoad,
             resolve_code(Sig{"StatsLoad", 0x3b3c140, "41 56 53 50 4c 8b 35 ?? ?? ?? ?? 48 89 fb 4c 89 f7 e8 ?? ?? ?? ?? 4c 89 f7"}))
BG3LE_TARGET(WwiseSetSwitch,
             resolve_code(Sig{"WwiseSetSwitch", 0x28a0b90, "50 48 89 f7 48 89 d6 48 89 ca e8 ?? ?? ?? ?? 83 f8 01"}))
BG3LE_TARGET(ParseFunctor,
             resolve_code(Sig{"ParseFunctor", 0x2b874c0, "55 41 57 41 56 41 55 41 54 53 48 81 ec f8 03 00 00 0f 57 c0 48 c7 84 24 e0 00 00 00 00 00 00 00"}))
BG3LE_TARGET(PathStep,
             resolve_code(Sig{"PathStep", 0x2c699c0, "55 41 57 41 56 41 55 41 54 53 48 83 ec 68 c7 87 bc 01 00 00 00 00 00 00"}))
BG3LE_TARGET(AddAction,
             resolve_code(Sig{"AddAction", 0x2cc5b70, "41 56 53 50 48 8b 87 70 01 00 00 48 89 fb"}))
BG3LE_TARGET(MachineUpdate,
             resolve_code(Sig{"MachineUpdate", 0x2d1d9f0, "41 56 53 50 80 7f 08 00 75 08 48 83 c4 08"}))
BG3LE_TARGET(ClientAppUpdate,
             resolve_code(Sig{"ClientAppUpdate", 0x2d1b9b0, "55 41 57 41 56 41 55 41 54 53 48 81 ec 68 01 00 00 49 89 fc 48 8b bf 30 01 00 00 49 89 f5"}))
BG3LE_TARGET(ServerMachineUpdate,
             resolve_code(Sig{"ServerMachineUpdate", 0x2c66a60, "55 41 57 41 56 41 55 41 54 53 48 83 ec 38 83 7f 24 00 75 1e 48 8b 7f 08 48 85 ff 74 06 48 8b 07 ff 50 28"}))
BG3LE_TARGET(StringKeysManagerLoad,
             resolve_code(Sig{"StringKeysManagerLoad", 0x2f9bb42, "48 8b 35 ?? ?? ?? ?? 31 d2 89 c8 f7 76 18 48 c1 e2 03"}))
BG3LE_TARGET(MakeSet,
             resolve_code(Sig{"MakeSet", 0x2fd0750, "55 41 57 41 56 41 55 41 54 53 48 83 ec 28 48 89 fb 8b 7e 20"}))
BG3LE_TARGET(SplitGroups,
             resolve_code(Sig{"SplitGroups", 0x2fddd90, "41 57 41 56 53 48 81 ec 80 00 00 00 48 8d 5c 24 48 0f 57 c0 48 8d 05 ?? ?? ?? ??"}))
BG3LE_TARGET(IsModded,
             resolve_code(Sig{"IsModded", 0x37675d0, "55 41 57 41 56 41 55 41 54 53 50 44 8b 77 14"}))
BG3LE_TARGET(CreateAction,
             resolve_code(Sig{"CreateAction", 0x38a6020, "55 41 57 41 56 41 55 41 54 53 50 48 89 cb 89 d5 49 89 fe"}))
BG3LE_TARGET(WwiseSetState,
             resolve_code(Sig{"WwiseSetState", 0x3dae1e0, "48 89 f7 48 85 d2 74 1c 80 3a 00 48 8d 35 ?? ?? ?? ??"}))
BG3LE_TARGET(GlobalSwitchesLoad,
             resolve_code(Sig{"GlobalSwitchesLoad", 0x3f5c210, "48 8b 35 ?? ?? ?? ?? 48 8b bb 58 01 00 00 f3 0f 10 86 b0 01 00 00"}))
BG3LE_TARGET(SetTargetState,
             resolve_code(Sig{"SetTargetState", 0x4017b00, "55 41 57 41 56 41 54 53 48 83 ec 20 4c 8b 25 ?? ?? ?? ?? 49 89 fe"}))
BG3LE_TARGET(VariableHelperVisit,
             resolve_code(Sig{"VariableHelperVisit", 0x41999e0, "55 41 57 41 56 41 54 53 48 83 ec 10 49 89 fe 48 8b be b0 00 00 00"}))
BG3LE_TARGET(LoadModuleExit,
             resolve_code(Sig{"LoadModuleExit", 0x64824b0, "c3 cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc c3 cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc c3 cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc b0 01 c3 cc cc cc cc cc cc cc cc cc cc cc cc cc 80 3d ?? ?? ?? ?? 00"}))
BG3LE_TARGET(UpdateMessagesFunc,
             resolve_code(Sig{"UpdateMessagesFunc", 0x70771a0, "83 bf 9c 07 00 00 00 0f 84 ?? ?? ?? ?? 55 41 57"}))
BG3LE_TARGET(StateNames,
             resolve_rip(Sig{"StateNames", 0x2d1d0c6, "48 8d 05 ?? ?? ?? ?? 4c 8b 04 d8 e9 ?? ?? ?? ?? 48 8d 05 ?? ?? ?? ?? 4a 8b 0c e8"}, 3, 7))
BG3LE_TARGET(LoadModuleExitSlot,
             [] { const std::uintptr_t b = resolve_rip(Sig{"LoadModuleExitSlot", 0x4013427, "4c 8d 05 ?? ?? ?? ?? 0f 57 c0 48 89 47 08 48 89 4f 10"}, 3, 7); return b == 0 ? 0 : b + 0x38; }())
BG3LE_TARGET(ClientAppUpdateSlot,
             [] { const std::uintptr_t b = resolve_rip(Sig{"ClientAppVtable", 0x3f619ac, "48 8d 05 ?? ?? ?? ?? 48 8d 0d ?? ?? ?? ?? 0f 57 c0 48 8d 7b 60 48 89 03 48 89 4b 08"}, 3, 7); return b == 0 ? 0 : b + 0x290; }())
BG3LE_TARGET(CursorControlVtable,
             resolve_rip(Sig{"CursorControlVtable", 0x413daa4, "48 8d 0d ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 48 89 08 48 8d 0d ?? ?? ?? ??"}, 3, 7))
BG3LE_TARGET(WwiseVtable,
             resolve_rip(Sig{"WwiseVtable", 0x3356b2d, "48 8d 05 ?? ?? ?? ?? 48 89 03 66 c7 83 d6 05 00 00 00 00"}, 3, 7))
BG3LE_TARGET(StateMachineVtable,
             resolve_rip(Sig{"StateMachineVtable", 0x3f63dcb, "48 8d 05 ?? ?? ?? ?? 66 0f ef c0 49 8d be 98 00 00 00"}, 3, 7))
BG3LE_TARGET(FunctorsVtable,
             resolve_rip(Sig{"FunctorsVtable", 0x2fb9045, "48 8d 15 ?? ?? ?? ?? 66 c7 83 90 00 00 00 00 00"}, 3, 7))
BG3LE_TARGET(UpdateMessagesSlot,
             [] { const std::uintptr_t b = resolve_rip(Sig{"UpdateMessagesSlot", 0x4011d2b, "4c 8d 05 ?? ?? ?? ?? 4c 8d bb 38 04 00 00 4c 89 2c ca"}, 3, 7); return b == 0 ? 0 : b + 0x110; }())
BG3LE_TARGET(EoCClient,
             resolve_rip(Sig{"EoCClient", 0x21eb97b, "48 8b 0d ?? ?? ?? ?? 49 8b 55 08 31 f6 48 8b 99 a0 01 00 00"}, 3, 7))
BG3LE_TARGET(EoCServer,
             resolve_rip(Sig{"EoCServer", 0x21bfa18, "48 8b 05 ?? ?? ?? ?? 48 8b 74 24 30 48 8b 80 88 02 00 00"}, 3, 7))
BG3LE_TARGET(StatsGlobal,
             resolve_rip(Sig{"StatsGlobal", 0x221d8d5, "48 8b 05 ?? ?? ?? ?? f3 0f 11 44 24 20 48 85 c0 0f 84 ?? ?? ?? ??"}, 3, 7))
BG3LE_TARGET(BoostsManager,
             resolve_rip(Sig{"BoostsManager", 0x33d70bc, "48 8b 0d ?? ?? ?? ?? 4c 8d 46 40 48 63 71 18 48 85 f6"}, 3, 7))
BG3LE_TARGET(ClientLevelManager,
             resolve_rip(Sig{"ClientLevelManager", 0x2245235, "48 8b 05 ?? ?? ?? ?? 48 8b 80 90 00 00 00 48 85 c0 0f 84 ?? ?? ?? ?? 48 8b 40 30"}, 3, 7))
BG3LE_TARGET(TemplatesCache,
             resolve_rip(Sig{"TemplatesCache", 0x2387b7d, "48 8b 0d ?? ?? ?? ?? 4c 8b 05 ?? ?? ?? ?? 0f b6 7e 04 e8 ?? ?? ?? ?? 48 85 c0 74 16"}, 3, 7))
BG3LE_TARGET(LevelManager,
             resolve_rip(Sig{"LevelManager", 0x2191ff7, "48 8b 35 ?? ?? ?? ?? 31 d2 4f 8d 6c 37 08 89 c8"}, 3, 7))
BG3LE_TARGET(SurfaceActionFactory,
             resolve_rip(Sig{"SurfaceActionFactory", 0x21d9f6c, "4c 8b 3d ?? ?? ?? ?? 48 63 7e 08 48 85 ff 0f 84 ?? ?? ?? ?? 48 63 0d ?? ?? ?? ?? 31 d2 48 89 c8 48 f7 f7 48 8b 06 48 63 d2 8b 14 90 85 d2 0f 88 ?? ?? ?? ?? 48 8b 46 20 89 d2"}, 3, 7))
BG3LE_TARGET(ParserContext,
             resolve_rip(Sig{"ParserContext", 0x284b29b, "48 8b 0d ?? ?? ?? ?? 41 89 c6 48 8d 7c 24 68 48 89 ee"}, 3, 7))
BG3LE_TARGET(TextureAtlasMap,
             resolve_rip(Sig{"TextureAtlasMap", 0x37d690f, "48 8b 3d ?? ?? ?? ?? 4c 89 74 24 20 44 89 f8 4c 89 6c 24 30"}, 3, 7))
BG3LE_TARGET(TemplatesManager,
             resolve_rip(Sig{"TemplatesManager", 0x2387b76, "48 8b 15 ?? ?? ?? ?? 48 8b 0d ?? ?? ?? ?? 4c 8b 05 ?? ?? ?? ?? 0f b6 7e 04 e8 ?? ?? ?? ?? 48 85 c0 74 16"}, 3, 7))
BG3LE_TARGET(ResourcesGlobal,
             resolve_rip(Sig{"ResourcesGlobal", 0x21d176e, "48 8b 05 ?? ?? ?? ?? 49 89 fc 49 89 d5 49 89 f6 48 8b b8 80 00 00 00"}, 3, 7))
BG3LE_TARGET(ResourcesGlobalLoad,
             resolve_code(Sig{"ResourcesGlobal", 0x21d176e, "48 8b 05 ?? ?? ?? ?? 49 89 fc 49 89 d5 49 89 f6 48 8b b8 80 00 00 00"}))
BG3LE_TARGET(PathRoots,
             resolve_rip(Sig{"PathRoots", 0x227028b, "4c 8d 25 ?? ?? ?? ?? 49 89 d6 48 89 fb 4b 8b 0c fc 0f b6 41 0f 84 c0 79 05 8b 41 08 eb 03 83 e0 7f 41 0f b6 4e 0f"}, 3, 7))
BG3LE_TARGET(InputManager,
             resolve_rip(Sig{"InputManager", 0x2563315, "48 8b 3d ?? ?? ?? ?? be 64 00 00 00 89 ca e8 ?? ?? ?? ?? f3 0f 10 4c 24 18"}, 3, 7))
BG3LE_TARGET(GlobalSwitches,
             resolve_rip(Sig{"GlobalSwitches", 0x21ec015, "48 8b 05 ?? ?? ?? ?? 44 8a bc 24 00 01 00 00 44 8b b4 24 f0 00 00 00"}, 3, 7))
BG3LE_TARGET(StringKeysManager,
             resolve_rip(Sig{"StringKeysManager", 0x2be55a5, "48 8b 35 ?? ?? ?? ?? 31 d2 89 e8 f7 76 18 48 c1 e2 03"}, 3, 7))
BG3LE_TARGET(TranslatedStringRepository,
             resolve_rip(Sig{"TranslatedStringRepository", 0x227ce15, "4c 8b 25 ?? ?? ?? ?? 0f 11 00 8b 84 24 88 05 00 00"}, 3, 7))
BG3LE_TARGET(TranslatedStringRepositoryLoad,
             resolve_code(Sig{"TranslatedStringRepository", 0x227ce15, "4c 8b 25 ?? ?? ?? ?? 0f 11 00 8b 84 24 88 05 00 00"}))
BG3LE_TARGET(FunctorExec1,
             resolve_code(Sig{"FunctorExec1", 0x3c7df30, "55 41 57 41 56 41 55 41 54 53 48 81 ec c8 04 00 00 49 89 f4 48 8d b2 40 01 00 00"}))
BG3LE_TARGET(FunctorExec2,
             resolve_code(Sig{"FunctorExec2", 0x3c7ed70, "55 41 57 41 56 41 55 41 54 53 48 81 ec c8 04 00 00 49 89 f4 48 8d b2 10 01 00 00"}))
BG3LE_TARGET(FunctorExec3,
             resolve_code(Sig{"FunctorExec3", 0x70a22d0, "55 41 57 41 56 41 55 41 54 53 48 81 ec 38 0c 00 00 49 89 d6"}))
BG3LE_TARGET(FunctorExec4,
             resolve_code(Sig{"FunctorExec4", 0x70a3530, "55 41 57 41 56 41 55 41 54 53 48 81 ec 18 08 00 00 49 89 d7"}))
BG3LE_TARGET(FunctorExec5,
             resolve_code(Sig{"FunctorExec5", 0x70c6fa0, "55 41 57 41 56 41 55 41 54 53 48 81 ec 78 09 00 00"}))
BG3LE_TARGET(FunctorExec6,
             resolve_code(Sig{"FunctorExec6", 0x70ce510, "55 41 57 41 56 41 55 41 54 53 48 81 ec 28 09 00 00 49 89 d5"}))
BG3LE_TARGET(FunctorExec7,
             resolve_code(Sig{"FunctorExec7", 0x70cf550, "55 41 57 41 56 41 55 41 54 53 48 81 ec 58 0a 00 00 49 89 d5"}))
BG3LE_TARGET(FunctorExec8,
             resolve_code(Sig{"FunctorExec8", 0x70d6660, "55 41 57 41 56 41 55 41 54 53 48 81 ec 98 09 00 00 48 89 d3"}))
BG3LE_TARGET(FunctorExec9,
             resolve_code(Sig{"FunctorExec9", 0x3c76910, "55 41 57 41 56 41 55 41 54 53 48 81 ec 08 08 00 00 49 89 cc"}))
BG3LE_TARGET(SpellPrototypeInit,
             resolve_code(Sig{"SpellPrototypeInit", 0x5e35a00, "55 41 57 41 56 41 55 41 54 53 48 81 ec e8 00 00 00 48 8b 1d ?? ?? ?? ?? 8b 47 08"}))
BG3LE_TARGET(StatusPrototypeInit,
             resolve_code(Sig{"StatusPrototypeInit", 0x27e4de0, "55 41 57 41 56 41 55 41 54 53 48 83 ec 38 4c 8b 2d ?? ?? ?? ?? 8b 47 08"}))
BG3LE_TARGET(InterruptPrototypeInit,
             resolve_code(Sig{"InterruptPrototypeInit", 0x2fc3420, "55 41 57 41 56 41 55 41 54 53 48 83 ec 18 8b 07 8b 6e 20"}))
BG3LE_TARGET(PassiveLoader,
             resolve_code(Sig{"PassiveLoader", 0x2fc1b50, "55 41 57 41 56 41 55 41 54 53 48 81 ec b8 00 00 00 80 7f 18 00"}))
BG3LE_TARGET(BoostParse,
             resolve_code(Sig{"BoostParse", 0x30317f0, "55 41 57 41 56 41 55 41 54 53 48 83 ec 58 85 f6"}))
BG3LE_TARGET(BoostInvoke,
             resolve_code(Sig{"BoostInvoke", 0x5e76320, "55 41 57 41 56 41 55 41 54 53 48 83 ec 68 48 89 fb 48 8b 7f 18"}))
BG3LE_TARGET(BoostCopy,
             resolve_code(Sig{"BoostCopy", 0x5e76670, "0f 10 46 18 48 89 d0 0f 11 42 18 0f 10 06 0f 11 02 48 8b 4e 10 48 89 4a 10 c3 cc cc cc cc cc cc 48 89 d0 48 85 d2 75 01 c3 0f 10 46 18 0f 11 40 18 0f 10 06 0f 11 00 48 8b 4e 10 48 89 48 10 eb e7 cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc 55 41 57 41 56 41 55 41 54 53 48 83 ec 48 8b 42 0c"}))
BG3LE_TARGET(BoostManage,
             resolve_code(Sig{"BoostManage", 0x5e76690, "48 89 d0 48 85 d2 75 01 c3 0f 10 46 18 0f 11 40 18 0f 10 06 0f 11 00 48 8b 4e 10 48 89 48 10 eb e7 cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc 55 41 57 41 56 41 55 41 54 53 48 83 ec 48 8b 42 0c"}))
BG3LE_TARGET(NetGetFreeMessage,
             resolve_code(Sig{"NetGetFreeMessage", 0x2380220, "55 41 57 41 56 41 55 41 54 53 50 48 8b 4f 08 48 63 c6 4c 8b 2c c1 4d 85 ed 0f 84 ?? ?? ?? ?? 4c 8d 77 20 48 89 fb 4c 89 f7"}))
BG3LE_TARGET(NetRegisterMessage,
             resolve_code(Sig{"NetRegisterMessage", 0x3ce78f0, "55 41 57 41 56 41 55 41 54 53 48 83 ec 28 49 89 fe bf 38 00 00 00"}))
BG3LE_TARGET(NetSendSinglePeer,
             resolve_code(Sig{"NetSendSinglePeer", 0x2b0636d, "cc cc cc 55 41 57 41 56 53 50 f0 ff 87 38 01 00 00", 3}))
BG3LE_TARGET(NetProtocolsReserve,
             resolve_code(Sig{"NetProtocolsReserve", 0x40cbf8f, "e9 ?? ?? ?? ?? cc cc cc cc cc cc cc cc cc cc cc cc 8b 47 08 48 39 f0 73 66", 17}))
BG3LE_TARGET(ThreadIndexRead,
             resolve_code(Sig{"ThreadIndexRead", 0x2b824e7, "e9 ?? ?? ?? ?? cc cc cc cc cc cc 55 41 57 41 56 41 55 41 54 53 50 83 ff ff", 66}))

}  // namespace bg3le::target
