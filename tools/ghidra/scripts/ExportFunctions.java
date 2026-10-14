// Exports the list of functions of the current program as TSV:
//   entry  size  min  max  name  source  callees(comma-separated list of entry points)
// "size" = max - entry + 1 (assumes a contiguous body; min/max give the actual body).
// Generic: used to carry symbols across versions (tools/symbols.py) and for the notes.
//
// Headless usage: -postScript ExportFunctions.java <output.tsv>
//@category GameCube

import java.io.File;
import java.io.PrintWriter;
import java.util.Set;
import java.util.TreeSet;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

public class ExportFunctions extends GhidraScript {

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 1) {
			printerr("ExportFunctions: the output path is required as an argument");
			return;
		}
		File out = new File(args[0]);
		out.getParentFile().mkdirs();
		int count = 0;
		try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
			w.println("# entry\tsize\tmin\tmax\tname\tsource\tcallees");
			FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
			while (it.hasNext() && !monitor.isCancelled()) {
				Function f = it.next();
				Address entry = f.getEntryPoint();
				AddressSetView body = f.getBody();
				Address min = body.getMinAddress();
				Address max = body.getMaxAddress();
				long size = max.getOffset() - entry.getOffset() + 1;
				Set<String> callees = new TreeSet<>();
				for (Function c : f.getCalledFunctions(monitor)) {
					callees.add(String.format("%08X", c.getEntryPoint().getOffset()));
				}
				w.printf("%08X\t%X\t%08X\t%08X\t%s\t%s\t%s%n", entry.getOffset(), size, min.getOffset(),
					max.getOffset(), f.getName(), f.getSymbol().getSource().toString(),
					String.join(",", callees));
				count++;
			}
		}
		println("ExportFunctions: " + count + " functions -> " + out);
	}
}
