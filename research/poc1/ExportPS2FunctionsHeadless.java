// Headless variant of Zmbor's ps2xRecomp/tools/ghidra/ExportPS2Functions.java.
//
// WHY A VARIANT EXISTS: the in-tree script is interactive (askFile for the
// TOML and CSV paths) and therefore cannot run under analyzeHeadless. This
// file keeps the same CSV contract the map loader consumes
// (Name,Start,End,Size; see ElfParser::loadGhidraMap in main) but takes the
// output path from the script arguments instead of a file dialog, and drops
// the interactive TOML emission (the TOML is trivially regenerated from the
// CSV by the caller; see run_poc1.sh).
//
// WHAT IT DOES NOT DO (deliberately, to keep the POC measurement honest):
// it does NOT re-implement the in-tree script's executable-label /
// materialised-address / data-pointer synthetic-entry collection. The POC
// therefore measures Ghidra's own function recovery (auto-analysis +
// r5900 language + installed EE extension analyzers), not a re-run of
// Zmbor's fallback heuristics inside Ghidra. A production headless port
// should port that collection too, and the comparison in validate_poc1.py
// will then show the delta.
//
// UNTESTED IN THIS ENVIRONMENT: no Java/Ghidra was available where this was
// written (see README.md blocker report). Do not treat this script as
// verified; run it once on a machine with Ghidra + the EE extension and
// record the result before relying on it.
//
// Usage (Ghidra 12.x, JDK 21):
//   analyzeHeadless <projDir> <projName> -import <elf> -overwrite \
//     -processor r5900:LE:32:default \
//     -scriptPath <this-dir> \
//     -postScript ExportPS2FunctionsHeadless.java <out.csv>
// The EE extension (ghidra-emotionengine-reloaded) must already be
// installed in the Ghidra installation; without it the r5900 language id
// does not exist and the import fails. That failure is itself a result:
// record it, do not silently fall back to generic MIPS.

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.FunctionManager;

import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

public class ExportPS2FunctionsHeadless extends GhidraScript {

    private static class FunctionRecord {
        String name;
        long start;
        long endExclusive;
        long size;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args == null || args.length < 1) {
            println("ERROR: usage: ExportPS2FunctionsHeadless.java <out.csv>");
            throw new IllegalArgumentException("missing output CSV path argument");
        }
        File csvFile = new File(args[0]);
        if (csvFile.getParentFile() != null) {
            csvFile.getParentFile().mkdirs();
        }

        FunctionManager fm = currentProgram.getFunctionManager();
        FunctionIterator it = fm.getFunctions(true);
        List<FunctionRecord> records = new ArrayList<>();
        while (it.hasNext() && !monitor.isCancelled()) {
            Function func = it.next();
            AddressSetView body = func.getBody();
            if (body == null || body.getNumAddresses() == 0) {
                continue;
            }
            FunctionRecord record = new FunctionRecord();
            record.name = func.getName();
            record.start = func.getEntryPoint().getOffset();
            record.endExclusive = body.getMaxAddress().getOffset() + 1L;
            record.size = body.getNumAddresses();
            records.add(record);
        }
        records.sort(Comparator.comparingLong(r -> r.start));

        try (PrintWriter writer = new PrintWriter(csvFile)) {
            writer.println("Name,Start,End,Size");
            for (FunctionRecord record : records) {
                writer.printf("%s,0x%08X,0x%08X,%d%n",
                    record.name, record.start, record.endExclusive, record.size);
            }
        }
        println(String.format("Exported %d functions to %s (program=%s, language=%s)",
            records.size(), csvFile.getAbsolutePath(), currentProgram.getName(),
            currentProgram.getLanguageID()));
    }
}
