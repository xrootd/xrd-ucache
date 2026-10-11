// Build a small file with two trees, as a NanoAOD file has them: Events (four branches, 2000
// entries, ZLIB) and Runs (two branches, 3 entries). For the case where one tree's record is
// damaged and a job reads only the other. Runs' baskets are written between Events' first and
// second clusters, on a page of their own away from the file's records.
//
//   root -l -b -q 'make_two_trees_fixture.C("two_trees_fixture.root")'
#include <TFile.h>
#include <TTree.h>

void make_two_trees_fixture(const char* path) {
  TFile out(path, "RECREATE", "", 101);
  TTree ev("Events", "Events");
  TTree runs("Runs", "Runs");
  int run = 1, nJet = 0;
  float pt[8];
  double x = 0;
  ev.Branch("run", &run, "run/I");
  ev.Branch("nJet", &nJet, "nJet/I");
  ev.Branch("Jet_pt", pt, "Jet_pt[nJet]/F");
  ev.Branch("x", &x, "x/D");
  ev.SetAutoFlush(1000);
  unsigned int r = 0;
  long long count = 0;
  runs.Branch("run", &r, "run/i");
  runs.Branch("genEventCount", &count, "genEventCount/L");
  for (int i = 0; i < 2000; ++i) {
    if (i == 1000) {
      for (unsigned int k = 1; k <= 3; ++k) {
        r = k;
        count = 1000 * k + 7;
        runs.Fill();
      }
      runs.FlushBaskets();
    }
    run = 1 + i / 1000;
    nJet = i % 5;
    for (int k = 0; k < nJet; ++k)
      pt[k] = 10.f + k + (i % 17) * 0.5f;
    x = i * 0.25;
    ev.Fill();
  }
  ev.Write();
  runs.Write();
}
