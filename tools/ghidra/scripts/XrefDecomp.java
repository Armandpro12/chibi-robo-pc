// Lists the references to addresses and decompiles functions, into a text file. Output meant for
// generated/ (contains game code: never copy it into the repository).
//
// Headless usage: -postScript XrefDecomp.java <output.txt> <request>...
//   xref:ADDR       references to ADDR (and to ADDR+1..ADDR+3), with the containing function
//   xrefr:A-B       references to any address of the range [A, B]
//   decomp:TARGET   C decompilation of the function containing TARGET (hexadecimal address or name)
//   str:TEXT        defined strings containing TEXT, and the references to each one
//@category GameCube

import java.io.File;
import java.io.PrintWriter;
import java.util.List;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.Symbol;

public class XrefDecomp extends GhidraScript {

	private DecompInterface decomp;

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 2) {
			printerr("XrefDecomp: <output> <request>...");
			return;
		}
		File out = new File(args[0]);
		out.getParentFile().mkdirs();
		try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
			for (int i = 1; i < args.length; i++) {
				String req = args[i];
				w.println("==== " + req);
				if (req.startsWith("xref:")) {
					Address a = toAddr(req.substring(5));
					for (int k = 0; k < 4; k++) {
						listRefs(w, a.add(k));
					}
				} else if (req.startsWith("xrefr:")) {
					String[] r = req.substring(6).split("-");
					Address a = toAddr(r[0]);
					Address b = toAddr(r[1]);
					for (Address x = a; x.compareTo(b) <= 0; x = x.add(1)) {
						listRefs(w, x);
					}
				} else if (req.startsWith("decomp:")) {
					decompile(w, req.substring(7));
				} else if (req.startsWith("str:")) {
					findStrings(w, req.substring(4));
				} else {
					w.println("unknown request");
				}
				w.println();
			}
		}
		if (decomp != null) {
			decomp.dispose();
		}
		println("XrefDecomp: " + (args.length - 1) + " requests -> " + out);
	}

	private void listRefs(PrintWriter w, Address target) {
		ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(target);
		while (it.hasNext()) {
			Reference r = it.next();
			Address from = r.getFromAddress();
			Function f = getFunctionContaining(from);
			w.println(target + " <- " + from + " " + r.getReferenceType() + " in "
				+ (f == null ? "?" : f.getName() + "@" + f.getEntryPoint()));
		}
	}

	private Function resolve(String target) {
		List<Function> byName = getGlobalFunctions(target);
		if (byName != null && !byName.isEmpty()) {
			return byName.get(0);
		}
		return getFunctionContaining(toAddr(target));
	}

	private void decompile(PrintWriter w, String target) {
		Function f = resolve(target);
		if (f == null) {
			w.println("no function at " + target);
			return;
		}
		if (decomp == null) {
			decomp = new DecompInterface();
			decomp.openProgram(currentProgram);
		}
		DecompileResults res = decomp.decompileFunction(f, 60, monitor);
		if (res == null || !res.decompileCompleted()) {
			w.println("decompilation failed: " + (res == null ? "?" : res.getErrorMessage()));
			return;
		}
		w.println("// " + f.getName() + " @ " + f.getEntryPoint());
		w.println(res.getDecompiledFunction().getC());
	}

	private void findStrings(PrintWriter w, String text) {
		DataIterator it = currentProgram.getListing().getDefinedData(true);
		while (it.hasNext()) {
			Data d = it.next();
			Object v = d.getValue();
			if (!(v instanceof String) || !((String) v).contains(text)) {
				continue;
			}
			w.println(d.getAddress() + " \"" + v + "\"");
			for (Reference r : getReferencesTo(d.getAddress())) {
				Function f = getFunctionContaining(r.getFromAddress());
				w.println("    <- " + r.getFromAddress() + " in "
					+ (f == null ? "?" : f.getName() + "@" + f.getEntryPoint()));
			}
			Symbol s = getSymbolAt(d.getAddress());
			if (s != null) {
				w.println("    symbol " + s.getName());
			}
		}
	}
}
