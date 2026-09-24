// In-game thumbnail generator: renders prefab previews from the installed pack files on a background thread.
// Nothing is spawned in the world and no game assets have to ship with the mod.
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <cstdint>
namespace thumbgen {
    struct CharInfo { uint32_t key = 0; std::string internal, name; };   // characterinfo row: spawn key, internal name, in-game name (may be empty)
    void Start();                                   // called once after the prefab index is loaded
    void Request(const std::string& prefabPath);    // render this prefab next (no-op when done or already queued)
    void Refresh(const std::string& prefabPath);    // render again even if it was done before
    bool Pending(const std::string& prefabPath);    // queued or currently rendering
    bool Processed(const std::string& prefabPath);  // rendered before (success or failure)
    bool Ready();                                   // pack index opened, worker running
    bool Idle();                                    // nothing queued and the background pass has reached the end (a few prefabs may stay unrenderable)
    void SetBackground(bool on);                    // false: only render what the browser asks for
    void SetQuality(int q);                         // 0 base colour, 1 + dye/tint, 2 + normal maps, 3 + specular/emissive (default); applies to new renders
    int  Quality();
    bool Background();
    int  Done();                                    // prefabs with a rendered image
    void WantNamesLanguage(const std::string& id);   // UI language id; the worker loads the in-game names for it
    std::shared_ptr<const std::unordered_map<std::string, std::string>> GameNames();   // prefab path -> in-game name (gimmicks), null until loaded
    std::shared_ptr<const std::vector<CharInfo>> Characters();   // all characters of characterinfo (NPC spawn list), null until loaded
    bool PassProgress(int* done, int* total);       // re-render pass of existing images (after a renderer fix): active, how far
    int  Failed();                                  // prefabs without usable geometry
    int  Total();
    int  Generation();                              // increments whenever a new png is written (texture cache retries on change)
    std::vector<std::string> TakeRefreshed();       // image files overwritten by a re-render (the texture cache drops them)
    bool Lz4Decode(const unsigned char* src, size_t n, std::vector<unsigned char>& out, size_t expect);   // LZ4 block (also used for the embedded prefab index)
    const char* Error();                            // "" or why the pack files could not be opened
}
