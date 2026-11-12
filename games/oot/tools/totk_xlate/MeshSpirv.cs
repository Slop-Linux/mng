// NVN2 task / mesh stages as Vulkan task / mesh shaders (VK_EXT_mesh_shader), the Vulkan counterpart of MeshStage.Wrap.
// The translator emits the stage as a compute shader working on its stage memory, a Workgroup uint array laid out by
// MeshStage (header, task payload input at word 4, outputs, primitive block). This rewrites that SPIR-V: the compute
// main becomes a function; a new entry point clears the stage memory, puts the workgroup's number (and a task's payload)
// in, calls it, then exports what the stage wrote: vertices, primitive attributes and triangle indices of a mesh shader,
// the payload and the mesh workgroup count of a task shader. The first workgroup of the draw comes in a push constant.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

sealed class MeshSpirv
{
    readonly List<uint[]> ins = new();   // the module's instructions after the header
    uint bound;
    readonly uint[] header = new uint[5];

    MeshSpirv(byte[] spv)
    {
        var w = new uint[spv.Length / 4]; Buffer.BlockCopy(spv, 0, w, 0, spv.Length);
        Array.Copy(w, header, 5); bound = w[3];
        for (int i = 5; i < w.Length;) { int n = (int)(w[i] >> 16); ins.Add(w[i..(i + n)]); i += n; }
    }
    static uint Op(uint[] i) => i[0] & 0xffff;
    static uint[] I(uint op, params uint[] a) { var r = new uint[a.Length + 1]; r[0] = (uint)(a.Length + 1) << 16 | op; Array.Copy(a, 0, r, 1, a.Length); return r; }
    static uint[] Str(string s) { var b = Encoding.UTF8.GetBytes(s + "\0"); var r = new uint[(b.Length + 3) / 4]; Buffer.BlockCopy(b, 0, r, 0, b.Length); return r; }
    uint Id() => bound++;

    // ---- the global section: types, constants and variables, deduplicated against what the module has ----
    int TypesEnd() { int k = ins.FindIndex(i => Op(i) == 54); return k < 0 ? ins.Count : k; }   // before the first OpFunction
    readonly List<uint[]> globals = new();
    uint FindOrAdd(Func<uint[], bool> match, Func<uint, uint[]> make, int at = 1)
    {
        foreach (var i in ins.Take(TypesEnd()).Concat(globals).ToList()) if (match(i)) return i[at];
        uint id = Id(); globals.Add(make(id)); return id;
    }
    uint Void() => FindOrAdd(i => Op(i) == 19, id => I(19, id));
    uint Bool() => FindOrAdd(i => Op(i) == 20, id => I(20, id));
    uint U32() => FindOrAdd(i => Op(i) == 21 && i[2] == 32 && i[3] == 0, id => I(21, id, 32, 0));
    uint F32() => FindOrAdd(i => Op(i) == 22 && i[2] == 32 && i.Length == 3, id => I(22, id, 32));
    uint Vec(uint t, uint n) => FindOrAdd(i => Op(i) == 23 && i[2] == t && i[3] == n, id => I(23, id, t, n));
    uint Ptr(uint sc, uint t) => FindOrAdd(i => Op(i) == 32 && i[2] == sc && i[3] == t, id => I(32, id, sc, t));
    uint Fn(uint ret) => FindOrAdd(i => Op(i) == 33 && i.Length == 3 && i[2] == ret, id => I(33, id, ret));
    uint CU(uint v) { uint t = U32(); return FindOrAdd(i => Op(i) == 43 && i.Length == 4 && i[1] == t && i[3] == v, id => I(43, t, id, v), 2); }   // OpConstant: result id in word 2
    uint CF(float v) { uint t = F32(), b = BitConverter.SingleToUInt32Bits(v); return FindOrAdd(i => Op(i) == 43 && i.Length == 4 && i[1] == t && i[3] == b, id => I(43, t, id, b), 2); }
    uint Arr(uint t, uint n) { uint len = CU(n); uint id = Id(); globals.Add(I(28, id, t, len)); return id; }   // a fresh array type (decorations may differ)

    readonly List<uint[]> decos = new();
    readonly List<uint> iface = new();
    uint Var(uint sc, uint type, string name = null)
    {
        uint id = Id(); globals.Add(I(59, Ptr(sc, type), id, sc));
        if (name != null) names.Add(I(5, new[] { id }.Concat(Str(name)).ToArray()));
        return id;
    }
    readonly List<uint[]> names = new();
    uint BuiltinInput(uint builtin, uint type)
    {
        foreach (var d in ins) if (Op(d) == 71 && d.Length == 4 && d[2] == 11 && d[3] == builtin) return d[1];   // OpDecorate BuiltIn
        uint v = Var(1, type); decos.Add(I(71, v, 11, builtin)); return v;
    }

    // ---- the new entry point's code ----
    readonly List<uint[]> code = new();
    uint Emit(uint op, uint type, params uint[] a) { uint id = Id(); code.Add(I(op, new[] { type, id }.Concat(a).ToArray())); return id; }
    void EmitV(uint op, params uint[] a) => code.Add(I(op, a));
    uint Label() { uint id = Id(); code.Add(I(248, id)); return id; }
    void Label(uint id) => code.Add(I(248, id));
    uint Load(uint t, uint p) => Emit(61, t, p);
    void Store(uint p, uint v) => EmitV(62, p, v);
    uint Add(uint a, uint b) => Emit(128, U32(), a, b);
    uint Mul(uint a, uint b) => Emit(132, U32(), a, b);
    uint UDiv(uint a, uint b) => Emit(134, U32(), a, b);
    uint UMod(uint a, uint b) => Emit(137, U32(), a, b);
    uint Shr(uint a, uint b) => Emit(194, U32(), a, b);
    uint And(uint a, uint b) => Emit(199, U32(), a, b);
    uint ULt(uint a, uint b) => Emit(176, Bool(), a, b);
    uint IEq(uint a, uint b) => Emit(170, Bool(), a, b);
    void Barrier() => EmitV(224, CU(2), CU(2), CU(0x108));   // OpControlBarrier Workgroup Workgroup AcquireRelease|WorkgroupMemory

    uint Min(uint a, uint b) { uint lt = ULt(a, b); return Emit(169, U32(), lt, a, b); }   // OpSelect
    /* for (n = start; n < end; n += step) body(n): structured loop */
    void For(uint start, uint end, uint step, Action<uint> body)
    {
        uint pre = cur, head = Id(), bodyL = Id(), cont = Id(), merge = Id(), next = Id();
        EmitV(249, head); Label(head);
        uint phi = Id(); int phiAt = code.Count; code.Add(null);
        uint cond = ULt(phi, end);
        EmitV(246, merge, cont, 0);   // OpLoopMerge, right before the branch
        EmitV(250, cond, bodyL, merge);
        Label(bodyL); cur = bodyL; body(phi); EmitV(249, cont);
        Label(cont); cur = cont; uint nx = Add(phi, step); EmitV(249, head);
        code[phiAt] = I(245, U32(), phi, start, pre, nx, cont);   // OpPhi
        Label(merge); cur = merge;
    }
    void If(uint cond, Action body)
    {
        uint then = Id(), merge = Id();
        EmitV(247, merge, 0); EmitV(250, cond, then, merge);
        Label(then); cur = then; body(); EmitV(249, merge);
        Label(merge); cur = merge;
    }
    uint cur;   // the block being written (for OpPhi)

    // ---- the conversion ----
    public static byte[] Convert(byte[] spv, MeshStage m, bool depthMinusW) => new MeshSpirv(spv).Run(m, depthMinusW);

    /* SPIR-V 1.4 has no BufferBlock: storage buffers become Block structs in the StorageBuffer class, with the pointers
       derived from them */
    void StorageBuffers()
    {
        var sb = new HashSet<uint>();
        for (int k = 0; k < ins.Count; k++) if (Op(ins[k]) == 71 && ins[k].Length == 3 && ins[k][2] == 3) { sb.Add(ins[k][1]); ins[k] = I(71, ins[k][1], 2); }   // BufferBlock -> Block
        if (sb.Count == 0) return;
        var ptrOf = new Dictionary<uint, uint>();   // Uniform pointer type -> its StorageBuffer twin
        /* the twin of a Uniform pointer type: declared at `at` (right before the variable that needs it), or with the other
           new globals (before the functions) */
        uint Twin(uint p, int at = -1)
        {
            if (ptrOf.TryGetValue(p, out uint t)) return t;
            var d = ins.First(i => Op(i) == 32 && i[1] == p);
            foreach (var i in ins.Concat(globals)) if (Op(i) == 32 && i[2] == 12 && i[3] == d[3]) return ptrOf[p] = i[1];
            uint id = Id(); var decl = I(32, id, 12, d[3]);
            if (at >= 0) ins.Insert(at, decl); else globals.Add(decl);
            return ptrOf[p] = id;
        }
        var vars = new HashSet<uint>();
        for (int k = 0; k < ins.Count; k++)
        {
            var i = ins[k];
            if (Op(i) != 59 || i[3] != 2) continue;
            var p = ins.First(t => Op(t) == 32 && t[1] == i[1]); if (!sb.Contains(p[3])) continue;
            int n = ins.Count; uint tw = Twin(i[1], k); k += ins.Count - n;   // a new declaration went in before it
            ins[k] = I(59, tw, i[2], 12); vars.Add(i[2]);
        }
        var derived = new HashSet<uint>(vars);
        for (int k = 0; k < ins.Count; k++)
        {
            var i = ins[k];
            if ((Op(i) == 65 || Op(i) == 66) && derived.Contains(i[3])) { var c = (uint[])i.Clone(); c[1] = Twin(i[1]); ins[k] = c; derived.Add(i[2]); }   // OpAccessChain / OpInBoundsAccessChain
        }
    }

    byte[] Run(MeshStage m, bool depthMinusW)
    {
        if (header[1] < 0x00010400) header[1] = 0x00010400;   // SPIR-V 1.4: VK_EXT_mesh_shader; every global in the interface
        StorageBuffers();
        int ep = ins.FindIndex(i => Op(i) == 15);
        uint oldMain = ins[ep][2];
        // the stage memory: the module's one Workgroup uint array
        uint mem = 0, memWords = 0;
        foreach (var v in ins) if (Op(v) == 59 && v[3] == 4)
            {
                var p = ins.First(t => Op(t) == 32 && t[1] == v[1]); var a = ins.First(t => Op(t) == 28 && t[1] == p[3]);
                var len = ins.First(t => Op(t) == 43 && t[2] == a[3]); mem = v[2]; memWords = len[3];
            }
        if (mem == 0) throw new Exception("mesh: no stage memory in the translation");
        bool task = m.Kind == "ts";
        uint u = U32(), f = F32(), v4 = Vec(f, 4), u3 = Vec(u, 3), pu = Ptr(4, u), T = (uint)m.Threads;
        uint lidVar = BuiltinInput(29, u), wgVar = BuiltinInput(26, u3);   // LocalInvocationIndex, WorkgroupId
        // the draw's first workgroup: push constant { uint first; }
        uint pcT = Id(); globals.Add(I(30, pcT, u)); decos.Add(I(71, pcT, 2)); decos.Add(I(72, pcT, 0, 35, 0));   // Block, Offset 0
        uint pc = Var(9, pcT, "dm_first");
        // the payload: TaskPayloadWorkgroupEXT uint[P]
        uint P = (uint)Math.Max((m.Payload + 3) / 4, 1), payload = 0;
        if (task || m.Task) payload = Var(5402, Arr(u, P), "dm_payload");
        uint layoutOut = (uint)m.Layout[2] / 4, prim = (uint)m.Layout[3];
        // mesh outputs
        uint posVar = 0, idxVar = 0; var vOut = new Dictionary<int, uint>(); var pOut = new Dictionary<int, uint>();
        if (!task)
        {
            uint blk = Id(); globals.Add(I(30, blk, v4)); decos.Add(I(71, blk, 2)); decos.Add(I(72, blk, 0, 11, 0));   // gl_MeshPerVertexEXT { Position }
            posVar = Var(3, Arr(blk, (uint)m.MaxV), "gl_MeshVerticesEXT");
            idxVar = Var(3, Arr(u3, (uint)m.MaxP), "gl_PrimitiveTriangleIndicesEXT"); decos.Add(I(71, idxVar, 11, 5296));
            foreach (int n in m.Vertex.Select(x => x.n).Where(n => n >= 0).Distinct()) { uint o = Var(3, Arr(v4, (uint)m.MaxV), $"dm_v{n}"); decos.Add(I(71, o, 30, (uint)n)); vOut[n] = o; }
            foreach (int n in m.Primitive.Select(x => x.n).Distinct()) { uint o = Var(3, Arr(v4, (uint)m.MaxP), $"dm_p{n}"); decos.Add(I(71, o, 30, (uint)n)); decos.Add(I(71, o, 5271)); pOut[n] = o; }
        }

        // the new main
        uint main = Id(), fnT = Fn(Void());
        code.Add(I(54, Void(), main, 0, fnT)); cur = Label();
        uint lid = Load(u, lidVar), wg = Emit(81, u, Load(u3, wgVar), 0);   // WorkgroupId.x
        uint MemAt(uint w) => Emit(65, pu, mem, w);   // OpAccessChain
        For(lid, CU(memWords), CU(T), i => Store(MemAt(i), CU(0)));
        Barrier();
        uint first = Load(u, Emit(65, Ptr(9, u), pc, CU(0)));
        if (!task && m.Task)
        {
            For(lid, CU(P), CU(T), i => Store(MemAt(Add(CU(4), i)), Load(u, Emit(65, Ptr(5402, u), payload, i))));
            Barrier();
            If(IEq(lid, CU(0)), () => { Store(MemAt(CU(0)), wg); Store(MemAt(CU(4)), wg); });
        }
        else If(IEq(lid, CU(0)), () => Store(MemAt(CU(0)), Add(first, wg)));
        Barrier();
        Emit(57, Void(), oldMain);   // OpFunctionCall
        Barrier();
        if (task)
        {
            For(lid, CU(P), CU(T), i => Store(Emit(65, Ptr(5402, u), payload, i), Load(u, MemAt(Add(CU(layoutOut), i)))));
            uint count = Min(Load(u, MemAt(CU(layoutOut + 1))), CU(1024));
            EmitV(5294, count, CU(1), CU(1), payload);   // OpEmitMeshTasksEXT (terminates)
        }
        else
        {
            uint np = Min(Shr(Load(u, MemAt(CU(prim / 4))), CU(24)), CU((uint)m.MaxP));
            EmitV(5295, CU((uint)m.MaxV), np);   // OpSetMeshOutputsEXT
            int stride = Math.Max(m.Vertex.Count + m.Primitive.Count, 1) * 32;
            uint Comp(List<(int n, int c)> comps, int n, int c, int offset, uint b)
            {
                int k = comps.IndexOf((n, c));
                return k < 0 ? CF(c == 3 ? 1f : 0f) : Emit(124, f, Load(u, MemAt(Add(b, CU((uint)((k + offset) * 32))))));   // OpBitcast
            }
            uint Block(uint n) => Add(Add(CU(layoutOut), Mul(UDiv(n, CU(32)), CU((uint)stride))), UMod(n, CU(32)));
            For(lid, CU((uint)m.MaxV), CU(T), n =>
            {
                uint b = Block(n);
                foreach (int loc in m.Vertex.Select(x => x.n).Distinct())
                {
                    uint x = Comp(m.Vertex, loc, 0, 0, b), y = Comp(m.Vertex, loc, 1, 0, b), z = Comp(m.Vertex, loc, 2, 0, b), w = Comp(m.Vertex, loc, 3, 0, b);
                    if (loc < 0 && depthMinusW) z = Emit(129, f, Emit(133, f, z, CF(0.5f)), Emit(133, f, w, CF(0.5f)));   // GL clip z to Vulkan: (z + w) / 2
                    uint vec = Emit(80, v4, x, y, z, w);   // OpCompositeConstruct
                    if (loc < 0) Store(Emit(65, Ptr(3, v4), posVar, n, CU(0)), vec);
                    else Store(Emit(65, Ptr(3, v4), vOut[loc], n), vec);
                }
            });
            if (pOut.Count > 0) For(lid, np, CU(T), n =>
            {
                uint b = Block(n);
                foreach (int loc in pOut.Keys)
                    Store(Emit(65, Ptr(3, v4), pOut[loc], n), Emit(80, v4, Comp(m.Primitive, loc, 0, m.Vertex.Count, b), Comp(m.Primitive, loc, 1, m.Vertex.Count, b),
                                                                Comp(m.Primitive, loc, 2, m.Vertex.Count, b), Comp(m.Primitive, loc, 3, m.Vertex.Count, b)));
            });
            For(lid, Mul(np, CU(3)), CU(T), n =>
            {
                uint at = Add(CU(prim + 4), n), word = Load(u, MemAt(Shr(at, CU(2))));
                uint idx = And(Shr(word, Mul(And(at, CU(3)), CU(8))), CU(255));
                Store(Emit(65, Ptr(3, u), idxVar, UDiv(n, CU(3)), UMod(n, CU(3))), idx);
            });
            EmitV(253);   // OpReturn
        }
        code.Add(I(56));   // OpFunctionEnd

        // the entry point, its modes, capabilities and interface (SPIR-V 1.4: every global variable the entry point uses)
        uint model = task ? 5364u : 5365u;   // TaskEXT / MeshEXT
        ins.RemoveAll(i => Op(i) == 16 && i[1] == ins[ep][2] && i[2] == 17);   // the old LocalSize (re-added for the new entry)
        foreach (var g in ins.Take(TypesEnd()).Concat(globals)) if (Op(g) == 59 && g[3] != 7) iface.Add(g[2]);
        var nameWords = Str("main");
        ins[ep] = I(15, new[] { model, main }.Concat(nameWords).Concat(iface.Distinct()).ToArray());
        var modes = new List<uint[]> { I(16, main, 17, T, 1, 1) };
        if (!task) { modes.Add(I(16, main, 26, (uint)m.MaxV)); modes.Add(I(16, main, 5270, (uint)m.MaxP)); modes.Add(I(16, main, 5298)); }   // OutputVertices, OutputPrimitivesEXT, OutputTrianglesEXT
        int afterEp = ins.FindIndex(i => Op(i) == 15) + 1; ins.InsertRange(afterEp, modes);
        int firstCap = ins.FindIndex(i => Op(i) == 17);
        ins.Insert(firstCap, I(17, 5283));   // MeshShadingEXT
        int extAt = ins.FindIndex(i => Op(i) == 10 || Op(i) == 11 || Op(i) == 14); ins.Insert(extAt, I(10, Str("SPV_EXT_mesh_shader")));
        int nameAt = ins.FindLastIndex(i => Op(i) == 5 || Op(i) == 6) + 1; ins.InsertRange(nameAt, names);
        int decoAt = ins.FindLastIndex(i => Op(i) >= 71 && Op(i) <= 72) + 1; ins.InsertRange(decoAt, decos);
        ins.InsertRange(TypesEnd(), globals);
        ins.AddRange(code);
        header[3] = bound;
        var outw = new List<uint>(header); foreach (var i in ins) outw.AddRange(i);
        var bytes = new byte[outw.Count * 4]; Buffer.BlockCopy(outw.ToArray(), 0, bytes, 0, bytes.Length);
        return bytes;
    }
}
