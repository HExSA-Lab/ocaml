(* TEST *)

(* With the default OCAMLRUNPARAM=f=1, values that are already old when
   they are stored into far memory are moved into far memory automatically,
   at the end of the next major GC cycle that finds them, without any call
   to Gc.compact. *)

external is_far_value : 'a -> bool = "caml_is_far_value"

let rec all_far = function
  | [] -> true
  | _ :: tl as l -> is_far_value l && all_far tl

let age x = Gc.full_major (); x

(* A full major collection (not a compaction) is enough. *)
let after_full_major () =
  let l = age (List.init 1000 string_of_int) in
  let big = age (Bytes.make 8192 'b') in
  let a = Array.make ~far:true 2 [] in
  let b = Array.make ~far:true 1 Bytes.empty in
  a.(0) <- l;
  b.(0) <- big;
  assert (not (is_far_value l));
  assert (not (is_far_value big));
  Gc.full_major ();
  assert (all_far a.(0));
  List.iteri (fun i s -> assert (is_far_value s); assert (s = string_of_int i))
    a.(0);
  assert (a.(0) == l);
  assert (is_far_value b.(0));
  assert (Bytes.get b.(0) 8191 = 'b')

(* No explicit GC call at all: a structure big enough to span many minor
   collections while it is built ends up far once the program has
   allocated enough for major cycles to run. *)
let no_gc_calls () =
  let a = Array.make ~far:true 1 [] in
  a.(0) <- List.init 200_000 (fun i -> i);
  let start = (Gc.quick_stat ()).major_collections in
  let garbage = ref [] in
  while not (all_far a.(0))
        && (Gc.quick_stat ()).major_collections < start + 20 do
    garbage := List.init 1000 Fun.id;
    ignore (Sys.opaque_identity !garbage)
  done;
  assert (all_far a.(0));
  List.iteri (fun i x -> assert (x = i)) a.(0)

(* Automatic migration across domains. *)
let domains () =
  let work d () =
    let a = Array.make ~far:true 1 [] in
    a.(0) <- age (List.init 500 (fun i -> d + i));
    Gc.full_major ();
    Gc.full_major ();
    assert (all_far a.(0));
    assert (a.(0) = List.init 500 (fun i -> d + i))
  in
  let ds = List.init 3 (fun d -> Domain.spawn (work (d * 1000))) in
  work 5000 ();
  List.iter Domain.join ds

let () =
  after_full_major ();
  no_gc_calls ();
  domains ()
