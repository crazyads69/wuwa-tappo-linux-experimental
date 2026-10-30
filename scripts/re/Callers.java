import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.*;
import ghidra.program.model.listing.*;
import java.io.*;
public class Callers extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    try (PrintWriter w = new PrintWriter(a[0])) {
      for (int i=1;i<a.length;i++) {
        w.println("CALLERS OF "+a[i]);
        for (Reference r : getReferencesTo(toAddr(a[i]))) { Function f=getFunctionContaining(r.getFromAddress()); w.println("  "+r.getReferenceType()+" from "+r.getFromAddress()+" fn "+(f==null?"?":f.getEntryPoint())); }
      }
    }
  }
}
