(* TEST *)

(* Values that are already old when they are stored into far memory are
   moved into far memory by compaction, together with the old values
   reachable from them. Both small (pool) and large blocks move, and the
   program cannot observe the move. *)

external is_far_value : 'a -> bool = "caml_is_far_value"

let rec all_far = function
  | [] -> true
  | _ :: tl as l -> is_far_value l && all_far tl

(* Make [x] old (and near). *)
let age x = Gc.full_major (); x

(* An old list, also referenced from near memory. *)
let old_list () =
  let l = age (List.init 1000 (fun i -> string_of_int i)) in
  assert (not (is_far_value l));
  let a = Array.make ~far:true 4 [] in
  a.(2) <- l;
  Gc.compact ();
  assert (all_far a.(2));
  List.iter (fun s -> assert (is_far_value s)) a.(2);
  assert (a.(2) == l);
  assert (List.nth l 999 = "999")

(* An old [init] value. *)
let old_init () =
  let r = age (ref (Some (String.make 10 'z'))) in
  let a = Array.make ~far:true 8 r in
  Gc.compact ();
  assert (is_far_value a.(7));
  assert (a.(0) == r);
  (match !r with
   | Some s -> assert (is_far_value s); assert (s = String.make 10 'z')
   | None -> assert false)

(* Large blocks (more than 256 words) are never young, so only compaction
   can move them. *)
let large () =
  let a =
    Array.init ~far:true 10 (fun i -> Bytes.make 4096 (Char.chr (65 + i)))
  in
  let near = a.(3) in
  Array.iter (fun b -> assert (not (is_far_value b))) a;
  Gc.compact ();
  Array.iteri (fun i b ->
      assert (is_far_value b);
      assert (Bytes.length b = 4096);
      assert (Bytes.get b 4095 = Char.chr (65 + i)))
    a;
  assert (near == a.(3));
  (* A large array reachable from a far array, with old small elements. *)
  let big = age (Array.init 1000 (fun i -> ref i)) in
  let f = Array.make ~far:true 1 big in
  Gc.compact ();
  assert (is_far_value f.(0));
  Array.iteri (fun i r -> assert (is_far_value r); assert (!r = i)) f.(0)

(* Closures, including mutually recursive ones (infix pointers). *)
let closures () =
  let n = age (ref 0) in
  let rec even k = k = 0 || odd (k - 1)
  and odd k = n := !n + 1; k <> 0 && even (k - 1) in
  ignore (age (even, odd));
  let a = Array.make ~far:true 3 (fun _ -> false) in
  a.(0) <- even;
  a.(1) <- odd;
  Gc.compact ();
  assert (is_far_value a.(0));
  assert (is_far_value a.(1));
  assert (is_far_value n);
  assert (a.(0) 10);
  assert (a.(1) 7);
  assert (!n > 0)

(* Custom blocks (boxed Int64) move too. *)
let customs () =
  let xs = age (List.init 100 (fun i -> Int64.of_int (i * 1_000_000_007))) in
  let a = Array.make ~far:true 1 xs in
  Gc.compact ();
  List.iteri (fun i x ->
      assert (is_far_value x);
      assert (x = Int64.of_int (i * 1_000_000_007)))
    a.(0);
  (* A custom block with a finaliser (which frees the data): it must only
     run for the live copy, once it is unreachable. *)
  let ba = age (Bigarray.Array1.init Bigarray.float64 Bigarray.c_layout
                  10_000 float_of_int) in
  let b = Array.make ~far:true 1 ba in
  Gc.compact ();
  assert (is_far_value b.(0));
  for _ = 1 to 3 do Gc.full_major () done;
  let ba = b.(0) in
  assert (Bigarray.Array1.get ba 9_999 = 9_999.);
  Bigarray.Array1.fill ba 1.;
  assert (Bigarray.Array1.get ba 0 = 1.);
  b.(0) <- Bigarray.Array1.create Bigarray.float64 Bigarray.c_layout 1;
  for _ = 1 to 3 do Gc.full_major () done

(* Mutation through a moved reference stays visible both ways. *)
let mutation () =
  let r = age (ref [1]) in
  let a = Array.make ~far:true 2 (ref []) in
  a.(1) <- r;
  Gc.compact ();
  assert (is_far_value r);
  r := [2; 3];
  assert (!(a.(1)) = [2; 3]);
  a.(1) := [4];
  assert (!r = [4])

(* Values reachable only from near memory stay near. *)
let near_only () =
  let l = age (List.init 100 Fun.id) in
  let a = age (Array.make 3 l) in
  Gc.compact ();
  assert (not (is_far_value l));
  assert (not (is_far_value a))

(* Moved values survive further collections and compactions. *)
let stable () =
  let a = Array.make ~far:true 100 [] in
  for i = 0 to 99 do a.(i) <- age (List.init 10 (fun j -> i + j)) done;
  for _ = 1 to 3 do
    Gc.compact ();
    Gc.full_major ()
  done;
  Array.iteri (fun i l ->
      assert (all_far l);
      assert (l = List.init 10 (fun j -> i + j)))
    a

(* Compaction moves values out of every domain's heap. *)
let domains () =
  let ready = Atomic.make 0 and go = Atomic.make false in
  let work d () =
    let l = List.init 100 (fun i -> d + i) in
    let a = Array.make ~far:true 1 [] in
    Gc.full_major ();
    a.(0) <- l;
    Atomic.incr ready;
    while not (Atomic.get go) do Domain.cpu_relax () done;
    assert (all_far a.(0));
    assert (a.(0) = List.init 100 (fun i -> d + i))
  in
  let ds = List.init 3 (fun d -> Domain.spawn (work (d * 1000))) in
  while Atomic.get ready < 3 do Domain.cpu_relax () done;
  Gc.compact ();
  Atomic.set go true;
  List.iter Domain.join ds

let () =
  old_list ();
  old_init ();
  large ();
  closures ();
  customs ();
  mutation ();
  near_only ();
  stable ();
  domains ()
