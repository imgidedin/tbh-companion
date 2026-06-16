// Applies Il2CppDumper script.json labels to the current GameAssembly.dll program.
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

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class ApplyIl2CppDumperLabelsHeadless extends GhidraScript {
	private static final String[] NAMED_FIELDS = {
		"ScriptMethod",
		"ScriptMetadata",
		"ScriptMetadataMethod"
	};

	private static final String[] KEYWORDS = {
		"StageManager",
		"Hero",
		"Monster",
		"Skill",
		"Projectile",
		"Battle",
		"Attack",
		"Target",
		"Move",
		"Dead",
		"Unit"
	};

	private int labelsApplied;
	private int labelFailures;
	private int functionsCreated;
	private int functionFailures;
	private final List<String> interestingMatches = new ArrayList<String>();

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 1) {
			printerr("Usage: ApplyIl2CppDumperLabelsHeadless.java <script.json> [labels-only|functions] [reportPath]");
			return;
		}

		String scriptJsonPath = args[0];
		boolean makeFunctions = args.length >= 2 && "functions".equalsIgnoreCase(args[1]);
		String reportPath = args.length >= 3 ? args[2] : null;

		JsonObject root;
		try (BufferedReader reader = Files.newBufferedReader(Paths.get(scriptJsonPath), StandardCharsets.UTF_8)) {
			root = JsonParser.parseReader(reader).getAsJsonObject();
		}

		int total = 0;
		for (String field : NAMED_FIELDS) {
			total += arraySize(root, field);
		}
		total += arraySize(root, "ScriptString");
		if (makeFunctions) {
			total += arraySize(root, "Addresses");
		}
		monitor.initialize(total);

		for (String field : NAMED_FIELDS) {
			processNamedArray(root, field);
		}
		processStrings(root);
		if (makeFunctions) {
			processFunctions(root);
		}

		printf("Il2CppDumper labels applied. labels=%d labelFailures=%d functions=%d functionFailures=%d matches=%d%n",
			labelsApplied, labelFailures, functionsCreated, functionFailures, interestingMatches.size());

		if (reportPath != null && reportPath.length() > 0) {
			writeReport(reportPath, scriptJsonPath, makeFunctions);
			printf("Report written: %s%n", reportPath);
		}
	}

	private int arraySize(JsonObject root, String key) {
		if (!root.has(key) || !root.get(key).isJsonArray()) {
			return 0;
		}
		return root.getAsJsonArray(key).size();
	}

	private void processNamedArray(JsonObject root, String field) throws Exception {
		if (!root.has(field) || !root.get(field).isJsonArray()) {
			return;
		}
		monitor.setMessage("Il2CppDumper: " + field);
		JsonArray items = root.getAsJsonArray(field);
		for (JsonElement item : items) {
			monitor.checkCancelled();
			JsonObject object = item.getAsJsonObject();
			long rva = object.get("Address").getAsLong();
			String name = getString(object, "Name");
			Address address = toAddress(rva);
			applyLabel(address, name);
			setComment(address, name);
			rememberInteresting(field, rva, name);
			monitor.incrementProgress(1);
		}
	}

	private void processStrings(JsonObject root) throws Exception {
		if (!root.has("ScriptString") || !root.get("ScriptString").isJsonArray()) {
			return;
		}
		monitor.setMessage("Il2CppDumper: ScriptString");
		JsonArray items = root.getAsJsonArray("ScriptString");
		int index = 1;
		for (JsonElement item : items) {
			monitor.checkCancelled();
			JsonObject object = item.getAsJsonObject();
			long rva = object.get("Address").getAsLong();
			String value = getString(object, "Value");
			Address address = toAddress(rva);
			applyLabel(address, "StringLiteral_" + index);
			setComment(address, truncate(value, 4000));
			rememberInteresting("ScriptString", rva, value);
			index++;
			monitor.incrementProgress(1);
		}
	}

	private void processFunctions(JsonObject root) throws Exception {
		if (!root.has("Addresses") || !root.get("Addresses").isJsonArray()) {
			return;
		}
		monitor.setMessage("Il2CppDumper: functions");
		JsonArray addresses = root.getAsJsonArray("Addresses");
		for (JsonElement item : addresses) {
			monitor.checkCancelled();
			Address address = toAddress(item.getAsLong());
			try {
				Function existing = getFunctionAt(address);
				if (existing == null) {
					disassemble(address);
					Function created = createFunction(address, null);
					if (created != null) {
						functionsCreated++;
					}
				}
			}
			catch (Exception e) {
				functionFailures++;
			}
			monitor.incrementProgress(1);
		}
	}

	private Address toAddress(long rva) {
		return currentProgram.getImageBase().add(rva);
	}

	private String getString(JsonObject object, String key) {
		if (!object.has(key) || object.get(key).isJsonNull()) {
			return "";
		}
		return object.get(key).getAsString();
	}

	private void applyLabel(Address address, String originalName) {
		String label = safeLabel(originalName);
		try {
			createLabel(address, label, true, SourceType.USER_DEFINED);
			labelsApplied++;
			return;
		}
		catch (Exception ignored) {
			// Fall through to offset-suffixed fallback.
		}

		try {
			String fallback = safeLabel(label + "_" + address.toString());
			createLabel(address, fallback, true, SourceType.USER_DEFINED);
			labelsApplied++;
		}
		catch (Exception e) {
			labelFailures++;
		}
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

	private void setComment(Address address, String comment) {
		try {
			setEOLComment(address, truncate(comment, 4000));
		}
		catch (Exception ignored) {
		}
	}

	private String truncate(String text, int maxLength) {
		if (text == null || text.length() <= maxLength) {
			return text;
		}
		return text.substring(0, maxLength) + "...";
	}

	private void rememberInteresting(String field, long rva, String text) {
		if (text == null) {
			return;
		}
		for (String keyword : KEYWORDS) {
			if (text.indexOf(keyword) >= 0) {
				interestingMatches.add(String.format("%s\t0x%X\t%s", field, rva, text));
				return;
			}
		}
	}

	private void writeReport(String reportPath, String scriptJsonPath, boolean makeFunctions) throws Exception {
		File reportFile = new File(reportPath);
		File parent = reportFile.getParentFile();
		if (parent != null) {
			parent.mkdirs();
		}

		try (PrintWriter writer = new PrintWriter(reportFile, StandardCharsets.UTF_8.name())) {
			writer.println("# TaskBarHero Ghidra IL2CPP Headless Report");
			writer.println();
			writer.println("program=" + currentProgram.getName());
			writer.println("imageBase=" + currentProgram.getImageBase());
			writer.println("scriptJson=" + scriptJsonPath);
			writer.println("makeFunctions=" + makeFunctions);
			writer.println("labelsApplied=" + labelsApplied);
			writer.println("labelFailures=" + labelFailures);
			writer.println("functionsCreated=" + functionsCreated);
			writer.println("functionFailures=" + functionFailures);
			writer.println("interestingMatches=" + interestingMatches.size());
			writer.println();
			writer.println("field\trva\tnameOrValue");
			for (String match : interestingMatches) {
				writer.println(match);
			}
		}
	}
}
