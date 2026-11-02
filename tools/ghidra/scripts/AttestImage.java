// Attests that the current program is the given DOL: every loaded block is compared with the DOL byte for byte, then
// the entry point, the language and the hash recorded at import. What it writes is what the program holds (the
// OBSERVED language, not the requested one), so tools/ghidra/analyze.py builds the provenance header of its outputs
// from this report. Read-only. Generic: no game-specific address or name here.
//
// Headless usage: -postScript AttestImage.java <main.dol> <expected language id, or -> <report.tsv>
// Report (TSV): "<key>\t<value>" lines, then one "block\t..." line per memory block and one "failure\t..." line per
// failed check; "result" is pass or fail.
//
// Uninitialized blocks hold no byte (BSS, hardware registers). An initialized block with bytes that are not from the DOL
// is a failure, except in the program of `import --rels`, not imported from the DOL: it also holds the RELs, whose
// relocated bytes cannot be compared with a file. They are listed, and analyze.py hashes the REL files instead.
//@category GameCube

import java.io.File;
import java.io.PrintWriter;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.HexFormat;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

import generic.jar.ResourceFile;
import ghidra.app.script.GhidraScript;
import ghidra.framework.Application;
import ghidra.framework.options.Options;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Program;
import ghidra.program.model.mem.MemoryBlock;

public class AttestImage extends GhidraScript {

	// DOL header (big-endian): 18 file offsets, 18 load addresses, 18 sizes (7 text then 11 data sections), BSS, entry.
	private static final int SECTIONS = 18;
	private static final int TEXT_SECTIONS = 7;
	private static final int ADDRESSES = 0x48;
	private static final int SIZES = 0x90;
	private static final int BSS_ADDRESS = 0xD8;
	private static final int BSS_SIZE = 0xDC;
	private static final int ENTRY = 0xE0;
	private static final int HEADER_SIZE = 0x100;

	// Same names as tools/ghidra/analyze.py: the loader the headless import forces, and the option ApplySymbolMap sets.
	private static final String EXTENSION = "GameCubeLoader";
	private static final String PROVENANCE_OPTIONS = "Provenance";

	private record Section(String name, long address, int offset, int size) {
		long end() {
			return address + size;
		}
	}

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 3) {
			printerr("AttestImage: expected <main.dol> <expected language id, or -> <report.tsv>");
			return;
		}
		File dolFile = new File(args[0]);
		String expectedLanguage = args[1];
		File reportFile = new File(args[2]);
		reportFile.getParentFile().mkdirs();

		byte[] dol = Files.readAllBytes(dolFile.toPath());
		Map<String, String> facts = new LinkedHashMap<>();
		List<String> blocks = new ArrayList<>();
		List<String> failures = new ArrayList<>();
		String dolHash = sha256(dol);

		observeProgram(facts);
		facts.put("dol_sha256", dolHash);
		facts.put("dol_name", dolFile.getName());
		if (dol.length < HEADER_SIZE) {
			failures.add("the DOL is shorter than its header");
		}
		else {
			compareBlocks(dol, importedFrom(dolFile.getName()), blocks, failures);
			checkEntry(dol, facts, failures);
		}
		checkLanguage(expectedLanguage, facts, failures);
		if (importedFrom(dolFile.getName()) && !dolHash.equalsIgnoreCase(currentProgram.getExecutableSHA256())) {
			failures.add("the program was imported from another file: SHA-256 " +
				currentProgram.getExecutableSHA256() + ", this DOL has " + dolHash);
		}

		String result = failures.isEmpty() ? "pass" : "fail";
		try (PrintWriter w = new PrintWriter(reportFile, "UTF-8")) {
			w.println("result\t" + result);
			facts.forEach((k, v) -> w.println(k + "\t" + v));
			blocks.forEach(w::println);
			failures.forEach(f -> w.println("failure\t" + f));
		}
		println(String.format("AttestImage: %s, %d blocks, %d failures -> %s", result, blocks.size(), failures.size(),
			reportFile));
	}

	/** What the program holds, read back from it rather than taken from the import command. */
	private void observeProgram(Map<String, String> facts) throws Exception {
		Options info = currentProgram.getOptions(Program.PROGRAM_INFO);
		facts.put("ghidra", Application.getApplicationVersion());
		facts.put("created_with", info.getString("Created With Ghidra Version", "unknown"));
		facts.put("language", currentProgram.getLanguageID().getIdAsString());
		facts.put("language_version", currentProgram.getLanguage().getVersion() + "." +
			currentProgram.getLanguage().getMinorVersion());
		facts.put("compiler", currentProgram.getCompilerSpec().getCompilerSpecID().getIdAsString());
		facts.put("format", currentProgram.getExecutableFormat());
		facts.put("executable_sha256", String.valueOf(currentProgram.getExecutableSHA256()));
		// Set by ApplySymbolMap at import: the map really applied, which may no longer be the file on disk today.
		// "unknown" for a project imported before ApplySymbolMap recorded it.
		facts.put("map_sha256", currentProgram.getOptions(PROVENANCE_OPTIONS).getString("map_sha256", "unknown"));
		observeExtension(facts);
	}

	private void observeExtension(Map<String, String> facts) throws Exception {
		String version = "unknown", jarHash = "unknown";
		ResourceFile root = Application.getModuleRootDir(EXTENSION);
		if (root != null) {
			File properties = new File(root.getFile(false), "extension.properties");
			if (properties.isFile()) {
				for (String line : Files.readAllLines(properties.toPath())) {
					if (line.startsWith("version=")) {
						version = line.substring("version=".length()).trim();
					}
				}
			}
			File jar = new File(root.getFile(false), "lib/" + EXTENSION + ".jar");
			if (jar.isFile()) {
				jarHash = sha256(Files.readAllBytes(jar.toPath()));
			}
		}
		facts.put("extension", EXTENSION + " " + version);
		facts.put("extension_jar_sha256", jarHash);
	}

	private static List<Section> sections(byte[] dol, List<String> failures) {
		ByteBuffer header = ByteBuffer.wrap(dol); // big-endian by default
		List<Section> out = new ArrayList<>();
		for (int i = 0; i < SECTIONS; i++) {
			int offset = header.getInt(i * 4);
			long address = Integer.toUnsignedLong(header.getInt(ADDRESSES + i * 4));
			int size = header.getInt(SIZES + i * 4);
			if (size == 0) {
				continue;
			}
			String name = i < TEXT_SECTIONS ? "text" + i : "data" + (i - TEXT_SECTIONS);
			if (offset < 0 || size < 0 || (long) offset + size > dol.length) {
				failures.add("section " + name + " lies outside the DOL");
				continue;
			}
			out.add(new Section(name, address, offset, size));
		}
		return out;
	}

	private boolean importedFrom(String fileName) {
		String imported = currentProgram.getExecutablePath();
		return imported != null && imported.toLowerCase().endsWith("/" + fileName.toLowerCase());
	}

	private void compareBlocks(byte[] dol, boolean fromDol, List<String> blocks, List<String> failures)
			throws Exception {
		List<Section> sections = sections(dol, failures);
		long[] sectionBytesSeen = new long[sections.size()];
		long bssStart = Integer.toUnsignedLong(ByteBuffer.wrap(dol).getInt(BSS_ADDRESS));
		long bssEnd = bssStart + Integer.toUnsignedLong(ByteBuffer.wrap(dol).getInt(BSS_SIZE));
		for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
			long start = block.getStart().getOffset();
			long end = start + block.getSize();
			long inDol = 0, differing = 0, firstDifference = -1;
			boolean loadedSpace = block.getStart().getAddressSpace() == currentProgram.getAddressFactory()
					.getDefaultAddressSpace();
			for (int i = 0; loadedSpace && i < sections.size(); i++) {
				Section s = sections.get(i);
				long lo = Math.max(start, s.address());
				long hi = Math.min(end, s.end());
				if (lo >= hi) {
					continue;
				}
				int length = (int) (hi - lo);
				inDol += length;
				sectionBytesSeen[i] += length;
				if (!block.isInitialized()) {
					differing += length;
					firstDifference = firstDifference < 0 ? lo : firstDifference;
					continue;
				}
				byte[] loaded = new byte[length];
				block.getBytes(toAddr(lo), loaded);
				int base = s.offset() + (int) (lo - s.address());
				for (int k = 0; k < length; k++) {
					if (loaded[k] != dol[base + k]) {
						differing++;
						firstDifference = firstDifference < 0 ? lo + k : firstDifference;
					}
				}
			}
			String status = inDol == 0 ? "not-in-dol"
					: differing > 0 ? "differs" : inDol == block.getSize() ? "match" : "match-partial";
			blocks.add(String.format("block\t%s\t%08X\t%X\t%s\t%s\t%X\t%X", block.getName(), start, block.getSize(),
				block.isInitialized() ? "initialized" : "uninitialized", status, inDol, differing));
			if (differing > 0) {
				failures.add(String.format("block %s: %X of %X bytes differ from the DOL, first at %08X",
					block.getName(), differing, inDol, firstDifference));
			}
			if (fromDol && block.isInitialized() && inDol != block.getSize()) {
				failures.add(String.format("block %s: %X bytes are not from the DOL", block.getName(),
					block.getSize() - inDol));
			}
		}
		for (int i = 0; i < sections.size(); i++) {
			Section s = sections.get(i);
			if (sectionBytesSeen[i] != s.size()) {
				failures.add(String.format("section %s: %X of %X bytes are in a memory block", s.name(),
					sectionBytesSeen[i], s.size()));
			}
		}
		checkBss(bssStart, bssEnd, sections, failures);
	}

	/** Every byte of the BSS range that no section fills must be mapped (the program can address it). */
	private void checkBss(long bssStart, long bssEnd, List<Section> sections, List<String> failures) {
		long unmapped = 0;
		for (long a = bssStart; a < bssEnd; a += 4) {
			long address = a;
			boolean filled = sections.stream().anyMatch(s -> s.address() <= address && address < s.end());
			if (!filled && !currentProgram.getMemory().contains(toAddr(a))) {
				unmapped++;
			}
		}
		if (unmapped > 0) {
			failures.add(String.format("BSS %08X-%08X: %X words are not in a memory block", bssStart, bssEnd, unmapped));
		}
	}

	private void checkEntry(byte[] dol, Map<String, String> facts, List<String> failures) {
		long entry = Integer.toUnsignedLong(ByteBuffer.wrap(dol).getInt(ENTRY));
		Address address = toAddr(entry);
		facts.put("dol_entry", String.format("%08X", entry));
		boolean isEntry = currentProgram.getSymbolTable().isExternalEntryPoint(address);
		if (!isEntry) {
			failures.add(String.format("the DOL's entry point %08X is not an entry point of the program", entry));
		}
	}

	private void checkLanguage(String expected, Map<String, String> facts, List<String> failures) {
		if (!expected.equals("-") && !expected.equals(facts.get("language"))) {
			failures.add("language " + facts.get("language") + " observed, " + expected + " expected");
		}
	}

	private static String sha256(byte[] data) throws Exception {
		return HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(data));
	}
}
