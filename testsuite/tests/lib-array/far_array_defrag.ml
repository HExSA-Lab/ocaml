(* TEST *)

(* Compaction also defragments far memory: live far blocks are packed into
   fewer far pools, and they stay in far memory. *)

external is_far_value : 'a -> bool = "caml_is_far_value"

let rec all_far = function
  | [] -> true
  | _ :: tl as l -> is_far_value l && all_far tl

let heap_words () = (Gc.quick_stat ()).heap_words

let () =
  (* Fill far pools with small arrays, then drop 15 out of 16 of them: every
     pool keeps a few live blocks, so sweeping alone cannot free any. *)
  let n = 1_000_000 in
  let all = Array.init n (fun i -> Array.make ~far:true 2 i) in
  let kept = Array.init (n / 16) (fun i -> all.(16 * i)) in
  Array.fill all 0 n [||];
  Gc.full_major ();
  let before = heap_words () in
  (* Also migrate an old near list of blocks of the same size into far
     memory during the same compaction, into the fragmented far pools. *)
  let l = List.init 100_000 (fun i -> i) in
  Gc.full_major ();
  let holder = Array.make ~far:true 1 [] in
  holder.(0) <- l;
  Gc.compact ();
  let after = heap_words () in
  assert (after < before / 2);
  Array.iteri (fun i a ->
      assert (is_far_value a);
      assert (a.(0) = 16 * i && a.(1) = 16 * i))
    kept;
  assert (all_far holder.(0));
  assert (holder.(0) == l);
  List.iteri (fun i x -> assert (x = i)) holder.(0);
  (* Defragmented far memory is reused. *)
  let again = Array.init (n / 2) (fun i -> Array.make ~far:true 2 (-i)) in
  Gc.compact ();
  Array.iteri (fun i a -> assert (is_far_value a); assert (a.(1) = -i)) again;
  Array.iteri (fun i a -> assert (a.(0) = 16 * i)) kept

(* Each domain's far memory is defragmented. *)
let () =
  let ready = Atomic.make 0 and go = Atomic.make false in
  let work d () =
    let all = Array.init 100_000 (fun i -> Array.make ~far:true 2 (d + i)) in
    let kept = Array.init 10_000 (fun i -> all.(10 * i)) in
    Array.fill all 0 (Array.length all) [||];
    Atomic.incr ready;
    while not (Atomic.get go) do Domain.cpu_relax () done;
    Array.iteri (fun i a ->
        assert (is_far_value a);
        assert (a.(1) = d + 10 * i))
      kept
  in
  let ds = List.init 3 (fun d -> Domain.spawn (work (d * 1_000_000))) in
  while Atomic.get ready < 3 do Domain.cpu_relax () done;
  Gc.compact ();
  Atomic.set go true;
  List.iter Domain.join ds
