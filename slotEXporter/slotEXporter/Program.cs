using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Synthesis;
using Mutagen.Bethesda.Skyrim;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Cache;
using Noggog;

namespace SlotExporter
{
    public class Program
    {
        private static readonly HashSet<ModKey> ProtectedMods = new()
        {
            ModKey.FromNameAndExtension("Skyrim.esm"),
            ModKey.FromNameAndExtension("Update.esm"),
            ModKey.FromNameAndExtension("Dawnguard.esm"),
            ModKey.FromNameAndExtension("HearthFires.esm"),
            ModKey.FromNameAndExtension("Dragonborn.esm")
        };

        public static async Task<int> Main(string[] args)
        {
            return await SynthesisPipeline.Instance
                .AddPatch<ISkyrimMod, ISkyrimModGetter>(RunPatch)
                .SetTypicalOpen(GameRelease.SkyrimSE, "SlotExport_Dummy.esp")
                .Run(args);
        }

        public static void RunPatch(IPatcherState<ISkyrimMod, ISkyrimModGetter> state)
        {
            Console.WriteLine("--- Slot Exporter (9-Column / Gender Split) Started ---");

            var outputDir = Path.Combine(state.DataFolderPath, "slotdataTXT");
            if (!Directory.Exists(outputDir)) Directory.CreateDirectory(outputDir);

            var exportLines = new HashSet<string>();

            foreach (var armor in state.LoadOrder.PriorityOrder.Armor().WinningOverrides())
            {
                if (ProtectedMods.Contains(armor.FormKey.ModKey)) continue;
                if (armor.Armature == null) continue;

                var armoSource = armor.FormKey.ModKey.FileName.String;
                var armoFormID = armor.FormKey.ID.ToString("X6");
                var armoEditorID = armor.EditorID ?? "NoEditorID";
                var armoSlots = ParseBodyFlags(armor.BodyTemplate?.FirstPersonFlags);

                foreach (var armaLink in armor.Armature)
                {
                    if (armaLink.TryResolve(state.LinkCache, out var arma))
                    {
                        var armaFormID = arma.FormKey.ID.ToString("X6");
                        var armaEditorID = arma.EditorID ?? "NoEditorID";
                        var armaSlots = ParseBodyFlags(arma.BodyTemplate?.FirstPersonFlags);

                        // ★修正: 男性・女性それぞれのパスを取得
                        string malePath = "";
                        string femalePath = "";

                        if (arma.WorldModel?.Male?.File != null) malePath = arma.WorldModel.Male.File;
                        if (arma.WorldModel?.Female?.File != null) femalePath = arma.WorldModel.Female.File;

                        // どちらかのパスが存在すれば出力
                        if (!string.IsNullOrWhiteSpace(malePath) || !string.IsNullOrWhiteSpace(femalePath))
                        {
                            // 9カラム形式
                            // Source;ARMA_F;ARMA_E;ARMO_F;ARMO_E;MalePath;FemalePath;ARMO_S;ARMA_S
                            string line = $"{armoSource};{armaFormID};{armaEditorID};{armoFormID};{armoEditorID};{malePath};{femalePath};{armoSlots};{armaSlots}";
                            exportLines.Add(line);
                        }
                    }
                }
            }

            var filePath = Path.Combine(outputDir, "slotdata-Exported.txt");
            var sb = new StringBuilder();
            foreach (var line in exportLines.OrderBy(x => x))
            {
                sb.AppendLine(line);
            }
            File.WriteAllText(filePath, sb.ToString());
            Console.WriteLine($"Exported {exportLines.Count} records (9-column format).");
        }

        private static string ParseBodyFlags(BipedObjectFlag? flags)
        {
            if (flags == null || flags == 0) return "";
            var numList = new List<string>();
            for (int i = 0; i < 32; i++)
            {
                uint mask = 1u << i;
                if (((uint)flags & mask) != 0) numList.Add((30 + i).ToString());
            }
            return string.Join(",", numList);
        }
    }
}