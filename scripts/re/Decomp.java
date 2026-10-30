import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.*;
public class Decomp extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();   // out-file addr...
    DecompInterface d = new DecompInterface();
    d.openProgram(currentProgram);
    try (PrintWriter w = new PrintWriter(a[0])) {
      for (int i=1;i<a.length;i++) {
        Function f = getFunctionContaining(toAddr(a[i]));
        if (f==null) { w.println("// no function at "+a[i]); continue; }
        DecompileResults r = d.decompileFunction(f, 120, monitor);
        w.println("// ===== "+f.getName()+" @ "+f.getEntryPoint());
        w.println(r.decompileCompleted()? r.getDecompiledFunction().getC() : "// FAILED "+r.getErrorMessage());
      }
    }
  }
}
