(* TEST *)

(* Elements of far arrays must be kept alive (and kept up to date) by the
   GC across minor collections, major collections and compaction. *)

external is_far_value : 'a -> bool = "caml_is_far_value"

let churn () =
  for _ = 1 to 200_000 do
    ignore (Sys.opaque_identity (ref 0))
  done

let collect () =
  churn ();
  Gc.minor ();
  Gc.full_major ();
  Gc.compact ();
  churn ();
  Gc.full_major ()

(* [init] is young when the array is created. *)
let young_init () =
  let a = Array.make ~far:true 1000 (ref 42) in
  assert (Array.is_far a);
  collect ();
  Array.iter (fun r -> assert (!r = 42)) a;
  assert (a.(0) == a.(999))

(* [init] is old and only reachable through the far array. *)
let old_init () =
  let a =
    let l = List.init 100 string_of_int in
    Gc.full_major ();
    Array.make ~far:true 10 l
  in
  collect ();
  Array.iter (fun l -> assert (List.nth l 57 = "57")) a

(* Elements stored after creation, through the write barrier. *)
let stored () =
  let a = Array.init ~far:true 1000 (fun i -> Some (string_of_int i)) in
  assert (Array.is_far a);
  collect ();
  Array.iteri (fun i x -> assert (x = Some (string_of_int i))) a

(* Mutation through a stored reference stays visible. *)
let mutation () =
  let a = Array.make ~far:true 4 (ref 0) in
  let r = ref 0 in
  a.(1) <- r;
  Gc.minor ();
  r := 5;
  assert (!(a.(1)) = 5);
  collect ();
  r := 6;
  assert (!(a.(1)) = 6);
  assert (a.(1) == r)

(* Large arrays are large allocations rather than pool blocks. *)
let large () =
  let a = Array.make ~far:true 100_000 (String.make 3 'x') in
  assert (is_far_value a);
  a.(99_999) <- String.make 3 'y';
  collect ();
  assert (a.(0) = "xxx");
  assert (a.(99_999) = "yyy")

let floats () =
  let a = Array.make ~far:true 1000 1.5 in
  assert (Array.is_far a);
  collect ();
  Array.iter (fun x -> assert (x = 1.5)) a;
  let c = Array.create_float ~far:true 10 in
  assert (Array.is_far c)

let not_far () =
  assert (not (Array.is_far [||]));
  assert (not (Array.is_far (Array.make ~far:true 0 0)));
  assert (not (Array.is_far (Array.make 3 0)));
  assert (not (is_far_value 3))

let () =
  young_init ();
  old_init ();
  stored ();
  mutation ();
  large ();
  floats ();
  not_far ()
