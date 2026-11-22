(* TEST
 ocamlrunparam += ",F=64M";
*)

(* Unreachable far arrays must be freed. The far region is limited to 64MB
   and the loops below allocate well over that in total, so they raise
   Out_of_memory unless far memory is reclaimed. *)

let () =
  (* Large allocations: 1MB each, 256MB in total. *)
  for i = 1 to 256 do
    let a = Array.make ~far:true (1024 * 1024 / 8 - 1) i in
    assert (Array.is_far a);
    ignore (Sys.opaque_identity a);
    if i mod 16 = 0 then Gc.full_major ()
  done;
  (* Pool blocks: 1M small arrays, about 128MB in total. *)
  for i = 1 to 1_000_000 do
    let a = Array.make ~far:true 15 i in
    ignore (Sys.opaque_identity a);
    if i mod 100_000 = 0 then Gc.full_major ()
  done
