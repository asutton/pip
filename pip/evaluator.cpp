#include "evaluator.hpp"
#include "action.hpp"
#include "expr.hpp"
#include "decode.hpp"
#include "type.hpp"
#include "decl.hpp"

#include <climits>

// testing only
#include <iostream>

namespace pip
{
  static cap::decoded_packet* create_modified_packet(const cap::packet& pkt)
  {
    cap::eth_header eth(pkt);

    // type == ipv4
    if(eth.ethertype = 0x800)
    {
      cap::ipv4_header ipv4(eth);

      // protocol == tcp
      if(ipv4.protocol == 6)
      {
	cap::tcp_header tcp(ipv4);
	return new cap::tcp_ipv4(eth, ipv4, tcp);
      }
	
    }

    throw std::runtime_error("Unsupported protocol.\n");
  }
  
  evaluator::evaluator(context& cxt, decl* prog, cap::packet& pkt)
    : cxt(cxt), 
      prog(prog), 
      data(pkt), 
      arrival(pkt.timestamp()),
      ingress_port(), 
      physical_port(), 
      metadata(), 
      keyreg(), 
      decode()
  {
    assert(cap::ethernet_ethertype(pkt.data()) == 0x800  &&
	   "Non-ethernet frames are not supported.\n");
    assert(cap::ipv4_protocol(pkt.data()) == 0x06 &&
	   "Non-TCP packets are not supported.\n");
    
    modified_buffer = new unsigned char[pkt.size()];
    std::memcpy(modified_buffer, pkt.data(), pkt.size());
    
    modified_data = create_modified_packet(pkt);
    ingress_port = cap::tcp_src_port(pkt.data());
    
    
    // TODO: Perform static initialization. We need to evaluate all of
    // the table definitions to load them with their static rules.
    auto program = static_cast<program_decl*>(prog);
    std::vector<decl*> tables;
    for(auto declaration : program->decls)
      if(auto t = dynamic_cast<table_decl*>(declaration))
	tables.push_back(t);

    // TODO: Load the instructions from the first table.
    current_table = static_cast<table_decl*>(tables.front());
    for(auto a : current_table->prep)
      eval.push_back(a);
  }

  evaluator::~evaluator()
  {
    if(modified_buffer)
      delete[] modified_buffer;
  }

  const action*
  evaluator::fetch()
  {
    const action* a = eval.front();
    if(a)
      std::cout << "there is an action in eval\n";
    eval.pop_front();
    return a;
  }

  void
  evaluator::step()
  {
    // If the action queue is empty, proceed to egress processing. This
    // will execute the actions in the action list. If the action list
    // is empty, then the program is complete.
    if (eval.empty()) {
      if (actions.empty())
        return;

      // This marks the beginning of egress processing.
      eval.insert(eval.end(), actions.begin(), actions.end());
      actions.clear();
    }
    
    const action* a = fetch();
    switch (get_kind(a)) {
      case ak_advance:
        return eval_advance(cast<advance_action>(a));
      case ak_copy:
        return eval_copy(cast<copy_action>(a));
      case ak_set:
        return eval_set(cast<set_action>(a));
      case ak_write:
        return eval_write(cast<write_action>(a));
      case ak_clear:
        return eval_clear(cast<clear_action>(a));
      case ak_drop:
        return eval_drop(cast<drop_action>(a));
      case ak_match:
        return eval_match(cast<match_action>(a));
      case ak_goto:
        return eval_goto(cast<goto_action>(a));
      case ak_output:
        return eval_output(cast<output_action>(a));
        
      default:
        break;   
    }

    throw std::logic_error("invalid action");
  }

  void
  evaluator::run()
  {
    while (!done())
      step();
  }

  void
  evaluator::eval_advance(const advance_action* a)
  {
    const auto n = static_cast<int_expr*>(a->amount);
    int amount = n->val;

    decode += amount;
  }

  // generate a bitmask over a specified range
  // credit to: ...
  template<typename R>
  static constexpr R
  bitfieldmask(unsigned int const a, unsigned int const b)
  {
    return ((static_cast<R>(-1) >> (((sizeof(R) * CHAR_BIT) - 1) - (b)))
	    & ~((1 << (a)) - 1));	 
  }

  // Copy n bits from position pos in a byte array into a 64-bit integer.
  static std::uint64_t
  data_to_key_reg(std::uint8_t* bytes, std::size_t pos, std::size_t n)
  {
    const std::uint8_t* ptr = bytes + pos / CHAR_BIT;
    std::uint64_t reg = 0;

    std::size_t offset = pos % CHAR_BIT;

    // If the copy begins between two whole bytes,
    if(offset > 0) {
      // copy the specified bits.
      reg = *(ptr++) & (0xFF >> (offset));

      // If there is nothing else to copy, shift off any excess and return.
      if(n <= CHAR_BIT - offset)
	return reg >> (CHAR_BIT - offset - n);
      // Remove the copied bits from n.
      else
	n -= CHAR_BIT - offset;
    }

    // Copy all remaining whole bytes.
    while(n >= CHAR_BIT) {
      reg = (reg << CHAR_BIT) + *(ptr++);
      n -= CHAR_BIT;
    }

    // If some fraction of a byte remains to be copied, copy it.
    if(n > 0)
      reg = (reg << n) + (*ptr >> (CHAR_BIT - n));
	
    return reg;
  }

  // Copy n bits from one byte array to another, starting at position pos.
  static void
  bitwise_copy(std::uint8_t* dst, std::uint8_t const * src,
	       std::size_t pos, std::size_t n)
  {
    std::uint8_t* ptr = dst + (pos / CHAR_BIT);
    std::size_t i = 0;

    // if the copy begins in between two whole bytes
    if(pos % CHAR_BIT > 0) {
      // copy just what is needed
      *ptr |= src[i] & (0xFF >> (pos % CHAR_BIT));    

      // if there is nothing else to copy, terminate
      if(n <= CHAR_BIT - pos % CHAR_BIT)
	return;
      // otherwise, even out n to start on the next whole byte
      // (n is now a multiple of CHAR_BIT)
      else
	n -= CHAR_BIT - pos % CHAR_BIT;

      ++ptr;
      // note that we have already accessed the first element of src
      ++i;
    }

    // copy all whole bytes that remain
    for(; n >= CHAR_BIT; n -= CHAR_BIT, ++i)
      *ptr++ = src[i];

    // if there is a remainder of less than CHAR_BIT bits,
    // copy just that portion of the byte.
    if(n > 0) {
      *ptr |= src[i] & (0xff << (CHAR_BIT - n));
    }  
  }
  
  void
  evaluator::eval_copy(const copy_action* a)
  {
    auto src_loc = static_cast<offset_expr*>(a->src);
    auto dst_loc = static_cast<offset_expr*>(a->dst);
    auto n = a->n->val;

    auto src_pos = static_cast<int_expr*>(src_loc->pos);
    auto src_len = static_cast<int_expr*>(src_loc->len);
    auto dst_pos = static_cast<int_expr*>(dst_loc->pos);
    auto dst_len = static_cast<int_expr*>(dst_loc->len);

    if(*(src_loc->space) == "key")
      throw std::runtime_error("Cannot copy from a key register.\n");

    if(n > dst_len->val || n > src_len->val)
      throw std::runtime_error("Copy action overflows buffer.\n");

    if(src_len->val != dst_len->val)
      throw std::runtime_error
	("Length of copy source and destination must be equal.\n");

    if(*(dst_loc->space) == "key") {
      if(*(src_loc->space) == "packet")
	keyreg = data_to_key_reg((std::uint8_t*)data.data(), src_pos->val, src_len->val);
      else if(*(src_loc->space) == "header")
	keyreg = data_to_key_reg((std::uint8_t*)data.data() + SIZE_ETHERNET, src_pos->val, src_len->val);
      else if(*(src_loc->space) == "meta")
	throw std::runtime_error("Unimplemented.\n");
    }
    else if(*(dst_loc->space) == "header") {
      if(*(src_loc->space) == "packet")
	bitwise_copy(modified_buffer + SIZE_ETHERNET, (std::uint8_t*)data.data(), dst_pos->val, dst_len->val);	
      else if(*(src_loc->space) == "meta")
	throw std::runtime_error("Unimplemented.\n");
    }
    else if(*(dst_loc->space) == "meta")
      return;
    else if(*(dst_loc->space) == "packet")
      if(*(src_loc->space) == "header")
	bitwise_copy(modified_buffer, packet_header(data), dst_pos->val, dst_len->val);
      return;
    
  }

  // Copy n bits from a uint64_t into a byte array at position pos.
  // See data_to_key_reg and bitwise_copy for further documentation.
  static void
  reg_to_buf(std::uint8_t* bytes, std::uint64_t in,
	     std::size_t pos, std::size_t n)
  {
    std::uint8_t* ptr = bytes + pos / CHAR_BIT;
    std::size_t offset = pos % CHAR_BIT;

    if(offset > 0) {
      *ptr++ = (std::uint8_t)in & (0xff >> offset);

      if(n <= CHAR_BIT - offset)
	return;
      else
	n -= CHAR_BIT - offset;

      in >>= offset;
    }

    while(n >= CHAR_BIT) {
      *ptr++ = (std::uint8_t)in & 0xff;
      in >>= 8;
      n -= CHAR_BIT;
    }

    if(n > 0)
      *ptr = (in << (CHAR_BIT - n)) + (*ptr >> n);
  }

  void
  evaluator::eval_set(const set_action* a)
  {
    const auto loc = static_cast<offset_expr*>(a->f);
    const auto pos_expr = static_cast<int_expr*>(loc->pos);

    const auto val_expr = static_cast<int_expr*>(a->v);

    std::uint64_t value = val_expr->val;
    std::size_t val_width = static_cast<int_type*>(val_expr->ty)->width;    
    std::size_t position = pos_expr->val;

    reg_to_buf(modified_buffer, value, position, val_width);
  }

  void
  evaluator::eval_write(const write_action* a)
  {
    eval.push_back(a);
  }

  void
  evaluator::eval_clear(const clear_action* a)
  {
    eval.clear();
  }

  void
  evaluator::eval_drop(const drop_action* a)
  {
    actions.clear();
  }

  void
  evaluator::eval_match(const match_action* a)
  {
    //TODO: Terminate after one match has been made?
    
    // If one of the rules matches the key register, then add
    // that rule's action list to the action list for egress processing.    
    for(auto r : current_table->rules)
    {
      auto key = r->key;
      std::cout << "keyreg: " << std::hex << keyreg << '\n';

      switch(get_kind(key))
      {
      case ek_int:
      {
	std::cout << "int rule matched\n";
	auto key_val = static_cast<int_expr*>(key)->val;
	if(keyreg == key_val)
	  for(auto a : r->acts)
	    actions.push_back(a);
	break;
      }
      case ek_port:
      {
	std::cout << "port rule matched\n";
	auto key_val = static_cast<port_expr*>(key)->port_num;
	if(keyreg == key_val)
	  for(auto a : r->acts)
	    actions.push_back(a);
	break;
      }
      case ek_miss:
      {
	std::cout << "missed\n";
	for(auto a : r->acts)
	  actions.push_back(a);
	break;
      }
      default:
	throw std::runtime_error("Rule key type does not match table kind.\n");
	break;
      }
    }
  }


  void
  evaluator::eval_goto(const goto_action* a)
  {
    // goto is a terminator action; eval must be empty at this point for this
    // program to be well formed.
    assert(eval.empty());
    
    decl* dst = static_cast<ref_expr*>(a->dest)->ref;
    current_table = static_cast<table_decl*>(dst);

    for(auto a : current_table->prep)
      eval.push_back(a);
  }

  void
  evaluator::eval_output(const output_action* a)
  {
    const port_expr* port = static_cast<port_expr*>(a->port);

    modified_data->set_output_port(port->port_num);
  }



} // namespace pip
