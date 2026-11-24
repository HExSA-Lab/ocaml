(* TEST
 ocamlrunparam += ",f=0";
*)

(* With OCAMLRUNPARAM=f=0, automatic far migration is disabled: old values
   stored into far memory stay near across major collections, and are only
   moved by Gc.compact. *)

external is_far_value : 'a -> bool = "caml_is_far_value"

let () =
  let l = List.init 1000 string_of_int in
  Gc.full_major ();
  let a = Array.make ~far:true 1 [] in
  a.(0) <- l;
  Gc.full_major ();
  Gc.full_major ();
  assert (not (is_far_value a.(0)));
  Gc.compact ();
  assert (is_far_value a.(0));
  assert (List.nth a.(0) 999 = "999")
