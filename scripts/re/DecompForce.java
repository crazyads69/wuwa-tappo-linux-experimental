import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import java.io.*;
// Like Decomp.java, but when no function contains the address it disassembles there and creates one
// (in-memory only: run with -readOnly). args: out-file addr...
public class DecompForce extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    DecompInterface d = new DecompInterface();
    d.openProgram(currentProgram);
    try (PrintWriter w = new PrintWriter(a[0])) {
      for (int i=1;i<a.length;i++) {
        Address ad = toAddr(a[i]);
        Function f = getFunctionContaining(ad);
        if (f==null) {
          new DisassembleCommand(ad, null, true).applyTo(currentProgram, monitor);
          f = createFunction(ad, null);
        }
        if (f==null) { w.println("// cannot create function at "+a[i]); continue; }
        DecompileResults r = d.decompileFunction(f, 180, monitor);
        w.println("// ===== "+f.getName()+" @ "+f.getEntryPoint()+" (asked "+a[i]+")");
        w.println(r.decompileCompleted()? r.getDecompiledFunction().getC() : "// FAILED "+r.getErrorMessage());
      }
    }
  }
}
