#include <catboost/libs/app_helpers/mode_fstr_helpers.h>
#include <catboost/libs/options/analytical_mode_params.h>

#include <library/getopt/small/last_getopt.h>

namespace {
    class TOpenSourceModeFstrImplementation : public NCB::IModeFstrImplementation {
        int mode_fstr(int argc, const char** argv) const override {
            NCB::TAnalyticalModeCommonParams params;
            auto parser = NLastGetopt::TOpts();

            NCB::PrepareFstrModeParamsParser(&params, &parser);

            NLastGetopt::TOptsParseResult parserResult{&parser, argc, argv};

            ModeFstrSingleHost(params);

            return 0;
        }
    };
}

NCB::TModeFstrImplementationFactory::TRegistrator<TOpenSourceModeFstrImplementation> YandexSpecificModeFstrImplementationRegistrator(NCB::EImplementationType::OpenSource);
