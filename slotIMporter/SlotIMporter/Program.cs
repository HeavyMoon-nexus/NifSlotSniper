using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Text;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Synthesis;
using Mutagen.Bethesda.Skyrim;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Cache;
using Noggog;

namespace SlotImporter
{
    public class Program
    {
        class SlotImportRecord
        {
            public string SourceModName { get; set; } = "";
            public string ArmaFormID { get; set; } = "";
            public string ArmoFormID { get; set; } = "";
            public string ArmaSlots { get; set; } = "";
            public string ArmoSlots { get; set; } = "";
        }

        public static async Task<int> Main(string[] args)
        {
            return await SynthesisPipeline.Instance
                .AddPatch<ISkyrimMod, ISkyrimModGetter>(RunPatch)
                .SetTypicalOpen(GameRelease.SkyrimSE, "SlotImporter_Log.esp")
                .Run(args);
        }

        public static void RunPatch(IPatcherState<ISkyrimMod, ISkyrimModGetter> state)
        {
            Console.WriteLine("--- Slot Importer (Replacer Generator) Started ---");

            var importRecords = LoadSlotData(state.DataFolderPath);

            if (importRecords.Count == 0)
            {
                Console.WriteLine("No valid update data found in 'Data/SlotdataTXT/slotdata-Output.txt'.");
                return;
            }

            int totalUpdated = 0;

            // 出力先
            var outputDir = Path.Combine(state.DataFolderPath, "GeneratedReplacers");
            if (!Directory.Exists(outputDir)) Directory.CreateDirectory(outputDir);

            Console.WriteLine($"Output Directory: {outputDir}");

            foreach (var group in importRecords.GroupBy(r => r.SourceModName))
            {
                var targetEspName = group.Key;
                var targetModKey = ModKey.FromNameAndExtension(targetEspName);

                if (!state.LoadOrder.TryGetValue(targetModKey, out var modListing) || modListing.Mod == null)
                {
                    Console.WriteLine($"  [Warning] Mod not found in Load Order: {targetEspName}. Skipped.");
                    continue;
                }

                Console.WriteLine($"Processing Target: {targetEspName}");

                var originalModGetter = modListing.Mod;
                ISkyrimMod mutableMod = (ISkyrimMod)originalModGetter.DeepCopy();
                int modUpdatedCount = 0;

                foreach (var rec in group)
                {
                    // ARMA
                    if (TryGetFormKey(rec.ArmaFormID, targetModKey, out var armaKey))
                    {
                        if (mutableMod.ArmorAddons.TryGetValue(armaKey, out var targetArma))
                        {
                            var newFlag = ParseSlots(rec.ArmaSlots);
                            if (targetArma.BodyTemplate?.FirstPersonFlags != newFlag)
                            {
                                targetArma.BodyTemplate ??= new BodyTemplate();
                                targetArma.BodyTemplate.FirstPersonFlags = newFlag;
                                modUpdatedCount++;
                            }
                        }
                    }

                    // ARMO
                    if (TryGetFormKey(rec.ArmoFormID, targetModKey, out var armoKey))
                    {
                        if (mutableMod.Armors.TryGetValue(armoKey, out var targetArmor))
                        {
                            var newFlag = ParseSlots(rec.ArmoSlots);
                            if (targetArmor.BodyTemplate?.FirstPersonFlags != newFlag)
                            {
                                targetArmor.BodyTemplate ??= new BodyTemplate();
                                targetArmor.BodyTemplate.FirstPersonFlags = newFlag;
                                modUpdatedCount++;
                            }
                        }
                    }
                }

                if (modUpdatedCount > 0)
                {
                    try
                    {
                        var outputPath = Path.Combine(outputDir, targetEspName);
                        mutableMod.WriteToBinary(outputPath);
                        Console.WriteLine($"  -> Generated Replacer: {targetEspName} ({modUpdatedCount} updates)");
                        totalUpdated += modUpdatedCount;
                    }
                    catch (Exception ex)
                    {
                        Console.WriteLine($"  [Error] Failed to write {targetEspName}: {ex.Message}");

                        if (ex.Message.Contains("Lower FormKey range") || ex.Message.Contains("FormKey"))
                        {
                            Console.WriteLine("    -> [Hint] This ESL/ESPFE mod likely contains invalid FormIDs (not compacted).");
                            Console.WriteLine("              Please run 'Compact FormIDs for ESL' in xEdit for this mod.");
                        }
                    }
                }
                else
                {
                    Console.WriteLine($"  -> No changes detected for {targetEspName}.");
                }
            }

            Console.WriteLine("==============================================================================");
            Console.WriteLine($"--- Completed: Total {totalUpdated} records updated across all mods. ---");

            // ★修正: 日本語を削除し、英語メッセージを強調
            Console.WriteLine("");
            Console.WriteLine("==============================================================================");
            Console.WriteLine(" [IMPORTANT] ");
            Console.WriteLine(" The generated .esp files are located in the 'GeneratedReplacers' folder.");
            Console.WriteLine(" Please MANUALLY move them to your 'Overwrite' folder (or enable via MO2).");
            Console.WriteLine("==============================================================================");
            Console.WriteLine("");
        }

        // --- ヘルパーメソッド ---

        private static List<SlotImportRecord> LoadSlotData(string dataPath)
        {
            var list = new List<SlotImportRecord>();

            string folderPath = Path.Combine(dataPath, "SlotdataTXT");
            string filePath = Path.Combine(folderPath, "slotdata-Output.txt");

            if (!File.Exists(filePath))
            {
                Console.WriteLine($"[Error] File not found: {filePath}");
                return list;
            }

            Console.WriteLine($"Loading file: {filePath}");

            try
            {
                var lines = File.ReadAllLines(filePath);
                foreach (var line in lines)
                {
                    if (string.IsNullOrWhiteSpace(line)) continue;

                    var parts = line.Split(';');
                    if (parts.Length < 9) continue;

                    list.Add(new SlotImportRecord
                    {
                        SourceModName = parts[0].Trim(),
                        ArmaFormID = parts[1].Trim(),
                        ArmoFormID = parts[3].Trim(),
                        ArmoSlots = parts[7].Trim(),
                        ArmaSlots = parts[8].Trim()
                    });
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[Error] Reading file: {ex.Message}");
            }

            Console.WriteLine($"Loaded {list.Count} records.");
            return list;
        }

        private static BipedObjectFlag ParseSlots(string slotStr)
        {
            BipedObjectFlag mask = 0;
            var slots = slotStr.Split(new[] { ' ', ',' }, StringSplitOptions.RemoveEmptyEntries);
            foreach (var s in slots)
            {
                if (int.TryParse(s.Trim(), out int sNum))
                {
                    if (sNum >= 30 && sNum <= 61) mask |= (BipedObjectFlag)(1u << (sNum - 30));
                }
            }
            return mask;
        }

        private static bool TryGetFormKey(string hexId, ModKey modKey, out FormKey key)
        {
            key = default;
            if (string.IsNullOrWhiteSpace(hexId)) return false;
            try
            {
                uint rawId = Convert.ToUInt32(hexId, 16);
                uint localId = rawId & 0x00FFFFFF;
                key = new FormKey(modKey, localId);
                return true;
            }
            catch { return false; }
        }
    }
}