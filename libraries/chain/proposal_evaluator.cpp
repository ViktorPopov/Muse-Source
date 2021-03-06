/*
 * Copyright (c) 2015 Cryptonomex, Inc., and contributors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <muse/chain/base_evaluator.hpp>
#include <muse/chain/proposal_object.hpp>
#include <muse/chain/account_object.hpp>
#include <muse/chain/exceptions.hpp>
#include <muse/chain/database.hpp>
#include <fc/smart_ref_impl.hpp>

namespace muse { namespace chain {
using namespace graphene::db;



void proposal_create_evaluator::do_apply(const proposal_create_operation& o)
{ try {
   database& d = db();
   transaction _proposed_trx;
   const auto& global_parameters = d.get_dynamic_global_properties();

   FC_ASSERT( o.expiration_time > d.head_block_time(), "Proposal has already expired on creation." );
   FC_ASSERT( o.expiration_time <= d.head_block_time() + global_parameters.maximum_proposal_lifetime,
              "Proposal expiration time is too far in the future.");
   FC_ASSERT( !o.review_period_seconds || fc::seconds(*o.review_period_seconds) < (o.expiration_time - d.head_block_time()),
              "Proposal review period must be less than its overall lifetime." );
   for( const op_wrapper& op : o.proposed_ops )
   {
      FC_ASSERT( !is_proposal_operation( op.op ), "Cannot propose a proposal");
      _proposed_trx.operations.push_back(op.op);
   }
   _proposed_trx.validate();

   const proposal_object& proposal = d.create<proposal_object>([&](proposal_object& proposal) {
      _proposed_trx.expiration = o.expiration_time;
      proposal.proposed_transaction = _proposed_trx;
      proposal.expiration_time = o.expiration_time;
      if( o.review_period_seconds )
         proposal.review_period_time = d.head_block_time() + *o.review_period_seconds;

      //Populate the required approval sets
      flat_set<string> required_active;
      vector<authority> other;
      
      for( auto& op : _proposed_trx.operations )
         operation_get_required_authorities(op, required_active, proposal.required_owner_approvals, proposal.required_basic_approvals, proposal.required_master_content_approvals, proposal.required_comp_content_approvals, other);
      
      //All accounts which must provide both owner and active authority should be omitted from the active authority set;
      //owner authority approval implies active authority approval.
      std::set_difference(required_active.begin(), required_active.end(),
                          proposal.required_owner_approvals.begin(), proposal.required_owner_approvals.end(),
                          std::inserter(proposal.required_active_approvals, proposal.required_active_approvals.begin()));
   });

} FC_CAPTURE_AND_RETHROW( (o) ) }

void proposal_update_evaluator::do_apply(const proposal_update_operation& o)
{ try {
   database& d = db();
   auto _proposal = &o.proposal(d);
   if( _proposal->review_period_time && d.head_block_time() <= *_proposal->review_period_time )
      FC_ASSERT( o.active_approvals_to_add.empty() && o.owner_approvals_to_add.empty(),
                 "This proposal is in its review period. No new approvals may be added." );
   for( const string& id : o.active_approvals_to_remove )
   {
      FC_ASSERT( _proposal->available_active_approvals.find(id) != _proposal->available_active_approvals.end(),
                 "", ("id", id)("available", _proposal->available_active_approvals) );
   }
   for( const string& id : o.owner_approvals_to_remove )
   {
      FC_ASSERT( _proposal->available_owner_approvals.find(id) != _proposal->available_owner_approvals.end(),
                 "", ("id", id)("available", _proposal->available_owner_approvals) );
   }


   // Potential optimization: if _executed_proposal is true, we can skip the modify step and make push_proposal skip
   // signature checks. This isn't done now because I just wrote all the proposals code, and I'm not yet 100% sure the
   // required approvals are sufficient to authorize the transaction.
   d.modify(*_proposal, [&o, &d](proposal_object& p) {
      p.available_active_approvals.insert(o.active_approvals_to_add.begin(), o.active_approvals_to_add.end());
      p.available_owner_approvals.insert(o.owner_approvals_to_add.begin(), o.owner_approvals_to_add.end());
      for( const string& id : o.active_approvals_to_remove )
         p.available_active_approvals.erase(id);
      for( const string& id : o.owner_approvals_to_remove )
         p.available_owner_approvals.erase(id);
      for( const auto& id : o.key_approvals_to_add )
         p.available_key_approvals.insert(id);
      for( const auto& id : o.key_approvals_to_remove )
         p.available_key_approvals.erase(id);
   });

   // If the proposal has a review period, don't bother attempting to authorize/execute it.
   // Proposals with a review period may never be executed except at their expiration.
   if( _proposal->review_period_time )
      return ;
   bool _executed_proposal = false;
   bool _proposal_failed = false;
   if( _proposal->is_authorized_to_execute(d) )
   {
      // All required approvals are satisfied. Execute!
      _executed_proposal = true;
      try {
         d.push_proposal(*_proposal);
      } catch(fc::exception& e) {
         wlog("Proposed transaction ${id} failed to apply once approved with exception:\n----\n${reason}\n----\nWill try again when it expires.",
              ("id", o.proposal)("reason", e.to_detail_string()));
         _proposal_failed = true;
      }
   }

} FC_CAPTURE_AND_RETHROW( (o) ) }
void proposal_delete_evaluator::do_apply(const proposal_delete_operation& o)
{ try {
   auto _proposal = &o.proposal(db());
   db().remove(*_proposal);

} FC_CAPTURE_AND_RETHROW( (o) ) }

} } // muse::chain
