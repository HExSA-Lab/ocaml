(* TEST *)

(* Young values stored in far arrays are promoted into far memory by the
   minor GC, together with the young values reachable from them.

   Only values that are entirely young when stored are guaranteed to end
   up far: if a minor GC happens while a multi-block value is being built
   (bytecode allocates each block separately), the part built so far is
   promoted into near memory through the stack. So before building such a
   value, the tests empty the minor heap with [Gc.minor ()]; a few words of
   allocation then cannot trigger a collection. *)

external is_far_value : 'a -> bool = "caml_is_far_value"

let rec all_far = function
  | [] -> true
  | _ :: tl as l -> is_far_value l && all_far tl

(* Elements stored through the write barrier. *)
let stored () =
  let a = Array.make ~far:true 100 [] in
  for i = 0 to Array.length a - 1 do
    Gc.minor ();
    a.(i) <- [i; i + 1; i + 2]
  done;
  Gc.minor ();
  Array.iteri (fun i l ->
      assert (l = [i; i + 1; i + 2]);
      assert (all_far l))
    a

(* A young [init] value. *)
let init () =
  Gc.minor ();
  let a = Array.make ~far:true 100 (Some (ref 0)) in
  Gc.minor ();
  (match a.(0) with
   | Some r -> assert (is_far_value a.(0)); assert (is_far_value r)
   | None -> assert false);
  assert (a.(0) == a.(99))

(* Young values that are also referenced from near memory: the far
   reference wins, and sharing is preserved. *)
let shared () =
  let a = Array.make ~far:true 10 [] in
  let near = Array.make 10 [] in
  Gc.full_major ();  (* make [near] old, so it is in the remembered set *)
  for i = 0 to 9 do
    let l = [i] in
    near.(i) <- l;
    a.(i) <- l
  done;
  Gc.minor ();
  for i = 0 to 9 do
    assert (is_far_value a.(i));
    assert (a.(i) == near.(i))
  done

(* Mutation through a stored reference stays visible, and values stored
   into a far value later are promoted into far memory too. *)
let mutation () =
  let a = Array.make ~far:true 4 (ref []) in
  let r = ref [] in
  a.(1) <- r;
  Gc.minor ();
  assert (is_far_value r);
  (* Not a literal: constant lists are statically allocated, not young. *)
  r := List.init 3 succ;
  Gc.minor ();
  assert (all_far !r);
  assert (!(a.(1)) = [1; 2; 3]);
  assert (a.(1) == r)

(* Values stored in near arrays stay near. *)
let near () =
  let a = Array.init 100 (fun i -> [i]) in
  Gc.minor ();
  Array.iter (fun l -> assert (not (is_far_value l))) a;
  assert (not (is_far_value a))

let () =
  stored ();
  init ();
  shared ();
  mutation ();
  near ();
  (* Promoted far values survive later collections. *)
  Gc.full_major ();
  Gc.compact ();
  stored ()
