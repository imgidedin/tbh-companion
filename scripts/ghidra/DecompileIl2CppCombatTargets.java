// Decompiles selected TaskbarHero IL2CPP combat methods from Il2CppDumper script.json.
//@category TaskBarHero

import java.io.BufferedReader;
import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

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

public class DecompileIl2CppCombatTargets extends GhidraScript {
	private static class MethodTarget {
		long rva;
		String name;
		String group;
	}

	private int selected;
	private int attempted;
	private int decompiled;
	private int failed;
	private int functionsCreated;
	private final List<String> summaryLines = new ArrayList<String>();

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 2) {
			printerr("Usage: DecompileIl2CppCombatTargets.java <script.json> <outputDir> [maxMethods]");
			return;
		}

		String scriptJsonPath = args[0];
		File outputDir = new File(args[1]);
		int maxMethods = args.length >= 3 ? Integer.parseInt(args[2]) : 350;
		outputDir.mkdirs();

		JsonObject root;
		try (BufferedReader reader = Files.newBufferedReader(Paths.get(scriptJsonPath), StandardCharsets.UTF_8)) {
			root = JsonParser.parseReader(reader).getAsJsonObject();
		}

		List<MethodTarget> targets = selectTargets(root, maxMethods);
		selected = targets.size();
		monitor.initialize(selected);

		DecompInterface decompiler = new DecompInterface();
		decompiler.openProgram(currentProgram);

		Map<String, PrintWriter> writers = new LinkedHashMap<String, PrintWriter>();
		try {
			for (MethodTarget target : targets) {
				monitor.checkCancelled();
				monitor.setMessage("Decompile " + target.name);
				PrintWriter writer = writerFor(writers, outputDir, target.group);
				decompileTarget(decompiler, target, writer);
				monitor.incrementProgress(1);
			}
		}
		finally {
			for (PrintWriter writer : writers.values()) {
				writer.close();
			}
			decompiler.dispose();
		}

		writeSummary(outputDir, scriptJsonPath, maxMethods);
		printf("Combat decompile finished. selected=%d attempted=%d decompiled=%d failed=%d functionsCreated=%d%n",
			selected, attempted, decompiled, failed, functionsCreated);
	}

	private List<MethodTarget> selectTargets(JsonObject root, int maxMethods) {
		List<MethodTarget> targets = new ArrayList<MethodTarget>();
		if (!root.has("ScriptMethod") || !root.get("ScriptMethod").isJsonArray()) {
			return targets;
		}

		JsonArray methods = root.getAsJsonArray("ScriptMethod");
		for (JsonElement item : methods) {
			JsonObject object = item.getAsJsonObject();
			String name = getString(object, "Name");
			String group = targetGroup(name);
			if (group == null) {
				continue;
			}

			MethodTarget target = new MethodTarget();
			target.rva = object.get("Address").getAsLong();
			target.name = name;
			target.group = group;
			targets.add(target);
			if (targets.size() >= maxMethods) {
				break;
			}
		}
		return targets;
	}

	private String targetGroup(String name) {
		if (name.startsWith("TaskbarHero.StageManager")) {
			return "StageManager";
		}
		if (name.startsWith("TaskbarHero.Hero$$") || name.startsWith("TaskbarHero.Unit$$")) {
			return "UnitHero";
		}
		if (name.startsWith("TaskbarHero.Monster$$")) {
			return "Monster";
		}
		if (name.startsWith("TaskbarHero.Combat.ActiveSkill") ||
			name.startsWith("TaskbarHero.Combat.HeroActiveSkill") ||
			name.startsWith("TaskbarHero.Combat.MonsterActive")) {
			return "ActiveSkill";
		}
		if (name.startsWith("TaskbarHero.Combat.ArcherBaseProjectile") ||
			name.startsWith("TaskbarHero.Combat.Projectile_Arrow") ||
			name.startsWith("TaskbarHero.Combat.ExplosionProjectile") ||
			name.startsWith("TaskbarHero.Combat.FrostBolt") ||
			name.startsWith("TaskbarHero.Combat.HunterFrostBolt") ||
			name.startsWith("TaskbarHero.Combat.ActBossCurvedProjectileBaseAttack") ||
			name.startsWith("TaskbarHero.Combat.bew$$") ||
			name.startsWith("TaskbarHero.Combat.beq$$") ||
			name.startsWith("TaskbarHero.Combat.bet$$") ||
			name.startsWith("TaskbarHero.Combat.bez$$") ||
			name.startsWith("TaskbarHero.Combat.bfa$$")) {
			return "Projectiles";
		}
		if (name.startsWith("TaskbarHero.Manager.MonsterSpawnManager")) {
			return "MonsterSpawnManager";
		}
		return null;
	}

	private void decompileTarget(DecompInterface decompiler, MethodTarget target, PrintWriter writer) {
		attempted++;
		Address address = currentProgram.getImageBase().add(target.rva);
		String safeName = safeLabel(target.name);

		writer.println();
		writer.println("// ============================================================");
		writer.println("// RVA: 0x" + Long.toHexString(target.rva).toUpperCase());
		writer.println("// Name: " + target.name);
		writer.println("// Address: " + address);

		try {
			createLabel(address, safeName, true, SourceType.USER_DEFINED);
		}
		catch (Exception ignored) {
		}

		Function function = getFunctionAt(address);
		if (function == null) {
			try {
				disassemble(address);
				function = createFunction(address, null);
				if (function != null) {
					functionsCreated++;
				}
			}
			catch (Exception e) {
				failed++;
				writer.println("// FAILED createFunction: " + e.getMessage());
				summaryLines.add("FAIL_CREATE\t0x" + Long.toHexString(target.rva).toUpperCase() + "\t" + target.name + "\t" + e.getMessage());
				return;
			}
		}

		if (function == null) {
			failed++;
			writer.println("// FAILED: function is null after createFunction");
			summaryLines.add("FAIL_NULL_FUNCTION\t0x" + Long.toHexString(target.rva).toUpperCase() + "\t" + target.name);
			return;
		}

		try {
			DecompileResults results = decompiler.decompileFunction(function, 30, monitor);
			if (results != null && results.decompileCompleted() && results.getDecompiledFunction() != null) {
				writer.println(results.getDecompiledFunction().getC());
				decompiled++;
				summaryLines.add("OK\t0x" + Long.toHexString(target.rva).toUpperCase() + "\t" + target.name);
			}
			else {
				failed++;
				String error = results == null ? "null results" : results.getErrorMessage();
				writer.println("// FAILED decompile: " + error);
				summaryLines.add("FAIL_DECOMPILE\t0x" + Long.toHexString(target.rva).toUpperCase() + "\t" + target.name + "\t" + error);
			}
		}
		catch (Exception e) {
			failed++;
			writer.println("// FAILED exception: " + e.getMessage());
			summaryLines.add("FAIL_EXCEPTION\t0x" + Long.toHexString(target.rva).toUpperCase() + "\t" + target.name + "\t" + e.getMessage());
		}
	}

	private PrintWriter writerFor(Map<String, PrintWriter> writers, File outputDir, String group) throws Exception {
		if (writers.containsKey(group)) {
			return writers.get(group);
		}
		File file = new File(outputDir, group + ".c");
		PrintWriter writer = new PrintWriter(file, StandardCharsets.UTF_8.name());
		writer.println("// Ghidra decompile output for " + group);
		writers.put(group, writer);
		return writer;
	}

	private void writeSummary(File outputDir, String scriptJsonPath, int maxMethods) throws Exception {
		File summary = new File(outputDir, "summary.tsv");
		try (PrintWriter writer = new PrintWriter(summary, StandardCharsets.UTF_8.name())) {
			writer.println("scriptJson\t" + scriptJsonPath);
			writer.println("maxMethods\t" + maxMethods);
			writer.println("selected\t" + selected);
			writer.println("attempted\t" + attempted);
			writer.println("decompiled\t" + decompiled);
			writer.println("failed\t" + failed);
			writer.println("functionsCreated\t" + functionsCreated);
			writer.println();
			writer.println("status\trva\tname\tdetail");
			for (String line : summaryLines) {
				writer.println(line);
			}
		}
	}

	private String getString(JsonObject object, String key) {
		if (!object.has(key) || object.get(key).isJsonNull()) {
			return "";
		}
		return object.get(key).getAsString();
	}

	private String safeLabel(String value) {
		String text = value == null ? "" : value;
		text = text.replace(' ', '_');
		text = text.replaceAll("[^0-9A-Za-z_.$@~-]", "_");
		text = text.replaceAll("_+", "_");
		text = text.replaceAll("^_+", "");
		text = text.replaceAll("_+$", "");
		if (text.length() == 0) {
			text = "Il2CppSymbol";
		}
		if (Character.isDigit(text.charAt(0))) {
			text = "_" + text;
		}
		if (text.length() > 240) {
			text = text.substring(0, 240);
		}
		return text;
	}
}
