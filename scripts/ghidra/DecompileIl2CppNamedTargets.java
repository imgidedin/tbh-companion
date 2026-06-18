//@category TaskBarHero

import java.io.BufferedReader;
import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class DecompileIl2CppNamedTargets extends GhidraScript {
	private static class MethodTarget {
		long rva;
		String name;
	}

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 3) {
			printerr("Usage: DecompileIl2CppNamedTargets.java <script.json> <outputFile> <contains...>");
			return;
		}

		String scriptJsonPath = args[0];
		File outputFile = new File(args[1]);
		List<String> needles = new ArrayList<String>();
		for (int i = 2; i < args.length; i++) {
			needles.add(args[i]);
		}

		JsonObject root;
		try (BufferedReader reader = Files.newBufferedReader(Paths.get(scriptJsonPath), StandardCharsets.UTF_8)) {
			root = JsonParser.parseReader(reader).getAsJsonObject();
		}

		List<MethodTarget> targets = selectTargets(root, needles);
		monitor.initialize(targets.size());

		DecompInterface decompiler = new DecompInterface();
		decompiler.openProgram(currentProgram);
		try (PrintWriter writer = new PrintWriter(outputFile, StandardCharsets.UTF_8.name())) {
			writer.println("// Named IL2CPP decompile output");
			for (MethodTarget target : targets) {
				monitor.checkCancelled();
				monitor.setMessage("Decompile " + target.name);
				decompileTarget(decompiler, target, writer);
				monitor.incrementProgress(1);
			}
		}
		finally {
			decompiler.dispose();
		}

		printf("Named decompile finished. selected=%d output=%s%n", targets.size(), outputFile.getAbsolutePath());
	}

	private List<MethodTarget> selectTargets(JsonObject root, List<String> needles) {
		List<MethodTarget> targets = new ArrayList<MethodTarget>();
		if (!root.has("ScriptMethod") || !root.get("ScriptMethod").isJsonArray()) {
			return targets;
		}
		JsonArray methods = root.getAsJsonArray("ScriptMethod");
		for (JsonElement item : methods) {
			JsonObject object = item.getAsJsonObject();
			String name = getString(object, "Name");
			for (String needle : needles) {
				if (name.contains(needle)) {
					MethodTarget target = new MethodTarget();
					target.rva = object.get("Address").getAsLong();
					target.name = name;
					targets.add(target);
					break;
				}
			}
		}
		return targets;
	}

	private void decompileTarget(DecompInterface decompiler, MethodTarget target, PrintWriter writer) {
		Address address = currentProgram.getImageBase().add(target.rva);
		writer.println();
		writer.println("// ============================================================");
		writer.println("// RVA: 0x" + Long.toHexString(target.rva).toUpperCase());
		writer.println("// Name: " + target.name);
		writer.println("// Address: " + address);
		try {
			createLabel(address, safeLabel(target.name), true, SourceType.USER_DEFINED);
		}
		catch (Exception ignored) {
		}
		Function function = getFunctionAt(address);
		if (function == null) {
			try {
				disassemble(address);
				function = createFunction(address, null);
			}
			catch (Exception e) {
				writer.println("// FAILED createFunction: " + e.getMessage());
				return;
			}
		}
		if (function == null) {
			writer.println("// FAILED: function is null");
			return;
		}
		try {
			DecompileResults results = decompiler.decompileFunction(function, 45, monitor);
			if (results != null && results.decompileCompleted() && results.getDecompiledFunction() != null) {
				writer.println(results.getDecompiledFunction().getC());
			}
			else {
				writer.println("// FAILED decompile: " + (results == null ? "null results" : results.getErrorMessage()));
			}
		}
		catch (Exception e) {
			writer.println("// FAILED exception: " + e.getMessage());
		}
	}

	private String getString(JsonObject object, String key) {
		if (!object.has(key) || object.get(key).isJsonNull()) return "";
		return object.get(key).getAsString();
	}

	private String safeLabel(String value) {
		String text = value == null ? "" : value;
		text = text.replace(' ', '_');
		text = text.replaceAll("[^0-9A-Za-z_.$@~-]", "_");
		text = text.replaceAll("_+", "_");
		text = text.replaceAll("^_+", "");
		text = text.replaceAll("_+$", "");
		if (text.length() == 0) text = "Il2CppSymbol";
		if (Character.isDigit(text.charAt(0))) text = "_" + text;
		return text.length() > 240 ? text.substring(0, 240) : text;
	}
}
