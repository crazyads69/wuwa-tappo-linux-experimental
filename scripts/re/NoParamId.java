import ghidra.app.script.GhidraScript;
public class NoParamId extends GhidraScript { public void run() throws Exception {
  setAnalysisOption(currentProgram, "Decompiler Parameter ID", "false"); setAnalysisOption(currentProgram, "Decompiler Switch Analysis", "false"); } }
