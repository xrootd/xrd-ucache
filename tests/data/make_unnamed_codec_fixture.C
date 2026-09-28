// Build a tiny TTree file whose compression setting names no algorithm while
// its baskets are compressed: the file and every branch record setting 1
// (algorithm 0, "the global default", level 1), which is what files written
// by older ROOT versions carry, and ROOT wrote the baskets with ZLIB. The
// last 1000 entries are fast-copied from a second tree written with LZMA, so
// each branch also holds baskets in a codec other than its first basket's.
//
//   root -l -b -q 'make_unnamed_codec_fixture.C("unnamed_codec_fixture.root")'
#include <TFile.h>
#include <TSystem.h>
#include <TTree.h>

#include <memory>

namespace {

void fill(TTree& t, int first, int n) {
  int run = 1, nJet = 0;
  float pt[8];
  double x = 0;
  t.Branch("run", &run, "run/I");
  t.Branch("nJet", &nJet, "nJet/I");
  t.Branch("Jet_pt", pt, "Jet_pt[nJet]/F");
  t.Branch("x", &x, "x/D");
  t.SetAutoFlush(1000);
  for (int i = first; i < first + n; ++i) {
    run = 1 + i / 1000;
    nJet = i % 5;
    for (int k = 0; k < nJet; ++k)
      pt[k] = 10.f + k + (i % 17) * 0.5f;
    x = i * 0.25;
    t.Fill();
  }
  t.ResetBranchAddresses();
}

} // namespace

void make_unnamed_codec_fixture(const char* path) {
  const char* lzmaPath = "unnamed_codec_fixture_lzma.tmp.root";
  {
    TFile f(lzmaPath, "RECREATE", "", 207);
    TTree t("Events", "Events");
    fill(t, 2000, 1000);
    t.Write();
  }
  TFile out(path, "RECREATE", "", 1);
  TTree t("Events", "Events");
  fill(t, 0, 2000);
  std::unique_ptr<TFile> in(TFile::Open(lzmaPath));
  t.CopyEntries(in->Get<TTree>("Events"), -1, "fast");
  out.cd();
  t.Write();
  in.reset();
  gSystem->Unlink(lzmaPath);
}
