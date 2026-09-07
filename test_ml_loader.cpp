// Test loader: load each packaged Zion model + run a dummy 88-feature Predict.
#include <cstdio>
#include "ml_model.h"

static void Test(const wchar_t* path, const char* name)
{
    CMLModel m;
    const bool loaded = m.Load(path);
    printf("%-22s Load=%s", name, loaded ? "ok" : "FAIL");
    if (!loaded) { printf("\n"); return; }
    printf("  feats=%u trees=%u", m.FeatureCount(), 0u); // TreeCount not exposed; skip

    float feats[88];
    for (int i = 0; i < 88; ++i) feats[i] = 0.5f + (i % 7) * 0.01f;

    float v = 0.0f;
    const bool pred = m.Predict(feats, 88, v);
    printf("  Predict=%s  value=%.4f\n", pred ? "ok" : "FAIL", v);
}

int main()
{
    const wchar_t* dir = L"C:\\Code\\VideoRenderer\\models\\";
    printf("=== dev models ===\n");
    Test((std::wstring(dir) + L"xgb_gamma_model.plxgb").c_str(), "gamma");
    Test((std::wstring(dir) + L"xgb_shadow_recovery.plxgb").c_str(), "shadow");
    Test((std::wstring(dir) + L"xgb_highlight_rolloff.plxgb").c_str(), "highlight");
    printf("=== HTPC bundle models ===\n");
    const wchar_t* hdir = L"C:\\htpc\\s3_shell_htpc\\mpcvr\\";
    Test((std::wstring(hdir) + L"xgb_gamma_model.plxgb").c_str(), "gamma(h)");
    Test((std::wstring(hdir) + L"xgb_shadow_recovery.plxgb").c_str(), "shadow(h)");
    Test((std::wstring(hdir) + L"xgb_highlight_rolloff.plxgb").c_str(), "highlight(h)");
    return 0;
}